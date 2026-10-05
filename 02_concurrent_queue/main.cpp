// Problem 2: Thread-safe Queue
//
// Wrap std::queue<int> so that multiple producers and consumers can use it.
//   void push(int);
//   bool try_pop(int&);   // false if the queue is empty
//
// Requirements: no lost elements, no duplicates, no deadlock, all threads terminate.
// Start with a mutex. Then ask: can this work without one?

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ---- A. Mutex-based queue ---------------------------------------------------
class ConcurrentQueue {
public:
    void push(int value) {
        std::scoped_lock lock(mtx);
        queue_.push(value);
    }

    bool try_pop(int& out) {
        std::scoped_lock lock(mtx);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<int> queue_;
    std::mutex mtx;
};

// ---- Harness ----------------------------------------------------------------
// Producer p pushes the values p * kItemsPerProducer + 0 .. kItemsPerProducer - 1,
// so every value is unique and encodes who produced it and in what order.
// Consumers pop until the total number of pops reaches the total number pushed.
constexpr int kItemsPerProducer = 500'000;
constexpr int kTimeoutSeconds = 15;

struct Result {
    double seconds;
    long long lost;
    long long duplicated;
    long long out_of_range;
    long long order_violations;  // a consumer saw one producer's items out of order
    bool ok() const { return !lost && !duplicated && !out_of_range && !order_violations; }
};

// If the run hasn't finished in time, assume deadlock / lost element and bail out.
void start_watchdog(int producers, int consumers) {
    std::thread([producers, consumers] {
        std::this_thread::sleep_for(std::chrono::seconds(kTimeoutSeconds));
        std::fprintf(stderr,
                     "TIMEOUT after %ds (%dP/%dC): deadlock, lost element, or consumers "
                     "never finishing\n",
                     kTimeoutSeconds, producers, consumers);
        std::_Exit(1);
    }).detach();
}

Result run(int producers, int consumers) {
    ConcurrentQueue queue;
    const long long total = static_cast<long long>(producers) * kItemsPerProducer;
    std::atomic<long long> consumed{0};
    std::vector<std::vector<int>> taken(consumers);  // what each consumer popped, in order
    for (auto& v : taken) v.reserve(total / consumers + 1024);

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&queue, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) queue.push(p * kItemsPerProducer + i);
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&queue, &consumed, &taken, total, c] {
            int value;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (queue.try_pop(value)) {
                    taken[c].push_back(value);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    auto end = std::chrono::steady_clock::now();

    // ---- Verify ----
    Result r{std::chrono::duration<double>(end - start).count(), 0, 0, 0, 0};
    std::vector<std::uint8_t> seen(total, 0);
    for (const auto& list : taken) {
        std::vector<int> last(producers, -1);  // last index seen per producer
        for (int value : list) {
            if (value < 0 || value >= total) {
                ++r.out_of_range;
                continue;
            }
            if (seen[value]++) ++r.duplicated;
            int producer = value / kItemsPerProducer;
            int index = value % kItemsPerProducer;
            if (index < last[producer]) ++r.order_violations;
            last[producer] = index;
        }
    }
    for (long long v = 0; v < total; ++v)
        if (!seen[v]) ++r.lost;
    return r;
}

int main() {
    struct Config {
        int producers, consumers;
    };
    for (Config c : {Config{1, 1}, Config{2, 2}, Config{4, 4}}) {
        start_watchdog(c.producers, c.consumers);
        Result r = run(c.producers, c.consumers);
        long long total = static_cast<long long>(c.producers) * kItemsPerProducer;
        std::printf("%dP/%dC time=%.4fs  items/s=%.3e  %s (lost=%lld dup=%lld range=%lld order=%lld)\n",
                    c.producers, c.consumers, r.seconds, total / r.seconds,
                    r.ok() ? "OK" : "WRONG", r.lost, r.duplicated, r.out_of_range,
                    r.order_violations);
    }
}
