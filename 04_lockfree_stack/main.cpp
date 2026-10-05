// Problem 4: Lock-free Stack (Treiber stack)
//
//   void push(int);
//   bool pop(int&);    // false if empty
//
// Build a linked list where the top of the stack is a std::atomic<Node*>:
//   push: new node -> node->next = current top -> swing top to node (compare_exchange)
//   pop:  read top -> swing top to top->next (compare_exchange) -> return its value
// A failed compare_exchange means another thread changed the top: retry.
//
// Memory reclamation: for now do NOT delete popped nodes (just leak them). Deleting
// them is unsafe: another thread may still be reading a node you just freed, and a
// freed address can be reused so the compare_exchange succeeds wrongly (the ABA
// problem). That is the next problem to attack once the basic algorithm works.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stack>
#include <thread>
#include <vector>

// ---- A. Lock-free stack -----------------------------------------------------
class LockFreeStack {
public:
    void push(int value) {
        Node* new_node = new Node;
        new_node->value = value;
        new_node->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(new_node->next, new_node,
                std::memory_order_release, std::memory_order_relaxed));
    }

    bool pop(int& out) {
        Node* old;
        while (true) {
            old = head.load(std::memory_order_acquire);
            if (old == nullptr) return false;
            if (head.compare_exchange_weak(old, old->next, 
                std::memory_order_relaxed, std::memory_order_relaxed)) break;
        }
        out = old->value;
        return true;
    }

private:
    struct Node { int value; Node* next; };
    std::atomic<Node*> head{nullptr};
};

// ---- Baseline: std::stack guarded by a mutex (same interface) ---------------
class MutexStack {
public:
    void push(int value) {
        std::scoped_lock lock(mtx_);
        stack_.push(value);
    }

    bool pop(int& out) {
        std::scoped_lock lock(mtx_);
        if (stack_.empty()) return false;
        out = stack_.top();
        stack_.pop();
        return true;
    }

private:
    std::stack<int> stack_;
    std::mutex mtx_;
};

// ---- Harness ----------------------------------------------------------------
constexpr int kItemsPerProducer = 500'000;
constexpr int kTimeoutSeconds = 30;

// Single-threaded sanity check: LIFO order, and pop on empty returns false.
template <typename Stack>
bool sanity_check(const char* name) {
    Stack s;
    int v = -1;
    if (s.pop(v)) {
        std::printf("%s sanity FAIL: pop on empty stack returned true\n", name);
        return false;
    }
    for (int i = 0; i < 10; ++i) s.push(i);
    for (int i = 9; i >= 0; --i) {
        if (!s.pop(v) || v != i) {
            std::printf("%s sanity FAIL: expected %d, got %s%d\n", name, i,
                        "value ", v);
            return false;
        }
    }
    if (s.pop(v)) {
        std::printf("%s sanity FAIL: stack should be empty again\n", name);
        return false;
    }
    std::printf("%s sanity OK (LIFO order, empty pop)\n", name);
    return true;
}

struct Result {
    double seconds;
    long long lost;
    long long duplicated;
    long long out_of_range;
    bool ok() const { return !lost && !duplicated && !out_of_range; }
};

std::atomic<int> g_run_id{0};

// Kills the process if the current run is still going after the timeout.
// Each new run bumps g_run_id, which disarms the previous run's watchdog.
void start_watchdog(const char* name, int producers, int consumers) {
    int id = ++g_run_id;
    std::thread([id, name, producers, consumers] {
        std::this_thread::sleep_for(std::chrono::seconds(kTimeoutSeconds));
        if (g_run_id.load() != id) return;
        std::fprintf(stderr, "TIMEOUT after %ds (%s %dP/%dC): hang or lost element\n",
                     kTimeoutSeconds, name, producers, consumers);
        std::_Exit(1);
    }).detach();
}

// An empty spin loop that never exits is undefined behavior; this keeps it well-defined.
inline void spin() { asm volatile("" ::: "memory"); }

// Producers push unique values; consumers pop until everything pushed has been popped.
// Afterwards every value must have been popped exactly once. (No cross-thread order
// check: a stack has no global order once threads interleave.)
template <typename Stack>
Result run(int producers, int consumers) {
    Stack stack;
    const long long total = static_cast<long long>(producers) * kItemsPerProducer;
    std::atomic<long long> consumed{0};
    std::vector<std::vector<int>> taken(consumers);
    for (auto& v : taken) v.reserve(total / consumers + 1024);

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&stack, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) stack.push(p * kItemsPerProducer + i);
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&stack, &consumed, &taken, total, c] {
            int value;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (stack.pop(value)) {
                    taken[c].push_back(value);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    spin();
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    auto end = std::chrono::steady_clock::now();

    Result r{std::chrono::duration<double>(end - start).count(), 0, 0, 0};
    std::vector<std::uint8_t> seen(total, 0);
    for (const auto& list : taken) {
        for (int value : list) {
            if (value < 0 || value >= total) {
                ++r.out_of_range;
                continue;
            }
            if (seen[value]++) ++r.duplicated;
        }
    }
    for (long long v = 0; v < total; ++v)
        if (!seen[v]) ++r.lost;
    return r;
}

template <typename Stack>
void report(const char* name, int producers, int consumers) {
    start_watchdog(name, producers, consumers);
    Result r = run<Stack>(producers, consumers);
    long long total = static_cast<long long>(producers) * kItemsPerProducer;
    std::printf("%-9s %dP/%dC time=%.4fs  items/s=%.3e  %s (lost=%lld dup=%lld range=%lld)\n",
                name, producers, consumers, r.seconds, total / r.seconds,
                r.ok() ? "OK" : "WRONG", r.lost, r.duplicated, r.out_of_range);
    std::fflush(stdout);  // the watchdog exits without flushing
}

int main() {
    if (!sanity_check<MutexStack>("mutex") || !sanity_check<LockFreeStack>("lockfree")) return 1;

    for (auto [p, c] : {std::pair{1, 1}, std::pair{2, 2}, std::pair{4, 4}, std::pair{8, 8}}) {
        report<MutexStack>("mutex", p, c);
        report<LockFreeStack>("lockfree", p, c);
    }
}
