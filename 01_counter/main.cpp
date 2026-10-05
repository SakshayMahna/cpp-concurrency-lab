// Problem 1: Thread-safe Counter
//
// 8 threads x 1,000,000 increments each -> final value must be exactly 8,000,000.
// Implement twice (mutex, atomic), verify, then benchmark at 1/2/4/8/16 threads.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

constexpr int kIncrementsPerThread = 1'000'000;

// ---- A. std::mutex ----------------------------------------------------------
class MutexCounter {
public:
    MutexCounter() : counter{0} {}

    void increment() {
        std::scoped_lock lock(mtx);
        counter += 1;
    }

    int get() const {
        std::scoped_lock lock(mtx);
        return counter;
    }

private:
    int counter;
    mutable std::mutex mtx;
};

// ---- B. std::atomic<int> ----------------------------------------------------
class AtomicCounter {
public:
    AtomicCounter() : counter{0} {}

    void increment() {
        counter.fetch_add(1, std::memory_order_relaxed);
    }
    void add(int n) {
        counter.fetch_add(n, std::memory_order_relaxed);
    }
    int get() const {
        return counter.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int> counter;
};

// ---- Harness ----------------------------------------------------------------
struct Result {
    double seconds;
    long long value;
};

template <typename Counter>
Result run(int num_threads) {
    Counter counter;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&counter] {
            for (int i = 0; i < kIncrementsPerThread; ++i) counter.increment();
        });
    }
    for (auto& th : threads) th.join();
    auto end = std::chrono::steady_clock::now();

    return {std::chrono::duration<double>(end - start).count(), counter.get()};
}

void print_result(const char* name, int num_threads, Result r) {
    long long expected = static_cast<long long>(num_threads) * kIncrementsPerThread;
    double ops_per_sec = static_cast<double>(expected) / r.seconds;
    std::printf("%-7s threads=%-2d time=%.4fs  ops/s=%.3e  %s (got %lld, expected %lld)\n",
                name, num_threads, r.seconds, ops_per_sec,
                r.value == expected ? "OK" : "WRONG", r.value, expected);
}

template <typename Counter>
void report(const char* name, int num_threads) {
    print_result(name, num_threads, run<Counter>(num_threads));
}

// ---- C. Thread-local accumulation (extra) -----------------------------------
// Each thread counts in a local variable and publishes once at the end, so the
// shared atomic is touched num_threads times instead of num_threads * 1M times.
Result run_local(int num_threads) {
    AtomicCounter total;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&total] {
            int local = 0;
            for (int i = 0; i < kIncrementsPerThread; ++i) {
                ++local;
                // Stop the compiler folding the loop into `local = 1'000'000`.
                asm volatile("" : "+r"(local));
            }
            total.add(local);
        });
    }
    for (auto& th : threads) th.join();
    auto end = std::chrono::steady_clock::now();

    return {std::chrono::duration<double>(end - start).count(), total.get()};
}

// ---- D. Sharded counter: one slot per thread (extra) ------------------------
// Unlike `local`, the total can be read at any time by summing the slots.
// Packed slots share cache lines (false sharing); padded slots get a line each.
constexpr std::size_t kCacheLine = 128;  // Apple silicon; 64 on most x86

struct PackedSlot {
    std::atomic<int> value{0};
};
struct alignas(kCacheLine) PaddedSlot {
    std::atomic<int> value{0};
};

template <typename Slot>
class ShardedCounter {
public:
    explicit ShardedCounter(int num_slots) : slots(num_slots) {}

    void increment(int slot) { slots[slot].value.fetch_add(1, std::memory_order_relaxed); }
    int get() const {
        int sum = 0;
        for (const auto& s : slots) sum += s.value.load(std::memory_order_relaxed);
        return sum;
    }

private:
    std::vector<Slot> slots;
};

template <typename Slot>
Result run_sharded(int num_threads) {
    ShardedCounter<Slot> counter(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&counter, t] {
            for (int i = 0; i < kIncrementsPerThread; ++i) counter.increment(t);
        });
    }
    for (auto& th : threads) th.join();
    auto end = std::chrono::steady_clock::now();

    return {std::chrono::duration<double>(end - start).count(), counter.get()};
}

int main() {
    for (int n : {1, 2, 4, 8, 16}) {
        report<MutexCounter>("mutex", n);
        report<AtomicCounter>("atomic", n);
        print_result("local", n, run_local(n));
        print_result("packed", n, run_sharded<PackedSlot>(n));
        print_result("padded", n, run_sharded<PaddedSlot>(n));
    }
}
