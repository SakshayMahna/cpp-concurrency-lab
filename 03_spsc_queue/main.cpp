// Problem 3: Lock-free SPSC Queue
//
// A fixed-capacity ring buffer for exactly ONE producer thread and ONE consumer thread.
//   bool try_push(const T&);   // false if full
//   bool try_pop(T&);          // false if empty
//
// Constraints: fixed capacity, no dynamic allocation after construction, no mutex.
// Think about: who writes head? who writes tail? full vs empty? which memory orders?

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <thread>

constexpr std::size_t kCacheLine = 128;

// ---- A. Lock-free SPSC queue ------------------------------------------------
template <typename T, std::size_t N>
class SPSCQueue {
public:

    bool try_push(const T& value) {
        const auto t = tail.load(std::memory_order_relaxed);
        if (t - cached_head == N) {
            cached_head = head.load(std::memory_order_acquire);
            if (t - cached_head == N) return false;
        }
        queue[t % N] = value;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const auto h = head.load(std::memory_order_relaxed);
        if (h == cached_tail) {
            cached_tail = tail.load(std::memory_order_acquire);
            if (h == cached_tail) return false;
        }
        out = queue[h % N];
        head.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    T queue[N];
    alignas(kCacheLine) std::atomic<std::size_t> head{0};
    std::size_t cached_tail{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail{0};
    std::size_t cached_head{0};
};

// ---- Baseline: bounded queue guarded by a mutex (same interface) ------------
template <typename T, std::size_t N>
class MutexQueue {
public:
    bool try_push(const T& value) {
        std::scoped_lock lock(mtx_);
        if (queue_.size() == N) return false;
        queue_.push(value);
        return true;
    }

    bool try_pop(T& out) {
        std::scoped_lock lock(mtx_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
};

// ---- Harness ----------------------------------------------------------------
// The producer pushes 0, 1, 2, ... and the consumer must receive exactly that
// sequence: no lost, no duplicated, no reordered, no garbage values.
constexpr long long kItems = 1'000'000;
constexpr int kTimeoutSeconds = 30;

struct Result {
    double seconds;
    long long mismatches;  // values that differed from the expected sequence
    long long received;
    bool ok() const { return mismatches == 0 && received == kItems; }
};

std::atomic<int> g_run_id{0};

// Kills the process if the current run is still going after the timeout.
// Each new run bumps g_run_id, which disarms the previous run's watchdog.
void start_watchdog(const char* name, std::size_t capacity) {
    int id = ++g_run_id;
    std::thread([id, name, capacity] {
        std::this_thread::sleep_for(std::chrono::seconds(kTimeoutSeconds));
        if (g_run_id.load() != id) return;
        std::fprintf(stderr, "TIMEOUT after %ds (%s, capacity=%zu): hang or lost element\n",
                     kTimeoutSeconds, name, capacity);
        std::_Exit(1);
    }).detach();
}

// An empty spin loop has no side effects, which is undefined behavior if it never
// exits (the compiler may delete or trap it). This keeps the loop well-defined.
inline void spin() { asm volatile("" ::: "memory"); }

template <typename Queue>
Result run() {
    Queue queue;
    Result r{0, 0, 0};

    auto start = std::chrono::steady_clock::now();
    std::thread producer([&queue] {
        for (long long i = 0; i < kItems; ++i)
            while (!queue.try_push(static_cast<int>(i))) spin();  // spin while full
    });
    std::thread consumer([&queue, &r] {
        int value;
        for (long long expected = 0; expected < kItems; ++expected) {
            while (!queue.try_pop(value)) spin();  // spin while empty
            if (value != static_cast<int>(expected)) ++r.mismatches;
            ++r.received;
        }
    });
    producer.join();
    consumer.join();
    auto end = std::chrono::steady_clock::now();

    r.seconds = std::chrono::duration<double>(end - start).count();
    return r;
}

template <typename Queue>
void report(const char* name, std::size_t capacity) {
    start_watchdog(name, capacity);
    Result r = run<Queue>();
    std::printf("%-6s capacity=%-6zu time=%.4fs  items/s=%.3e  %s (mismatches=%lld received=%lld)\n",
                name, capacity, r.seconds, kItems / r.seconds, r.ok() ? "OK" : "WRONG",
                r.mismatches, r.received);
    std::fflush(stdout);  // the watchdog exits without flushing
}

int main() {
    // Small capacities force constant wrap-around and full/empty transitions.
    report<MutexQueue<int, 2>>("mutex", 2);
    report<SPSCQueue<int, 2>>("spsc", 2);
    report<MutexQueue<int, 16>>("mutex", 16);
    report<SPSCQueue<int, 16>>("spsc", 16);
    report<MutexQueue<int, 1024>>("mutex", 1024);
    report<SPSCQueue<int, 1024>>("spsc", 1024);
    report<MutexQueue<int, 65536>>("mutex", 65536);
    report<SPSCQueue<int, 65536>>("spsc", 65536);
}
