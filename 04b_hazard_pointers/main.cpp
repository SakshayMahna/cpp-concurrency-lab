// Problem 4b: Safe memory reclamation with hazard pointers
//
// Same Treiber stack as problem 4, but popped nodes must now be FREED, and freeing must
// be safe: no use-after-free, no ABA, no double free, and no leaks.
//
// Hazard pointer idea:
//   * every thread owns one "hazard slot" (a std::atomic<Node*>, visible to everyone)
//   * before dereferencing a node, publish it in your slot, then re-check it is still
//     the node you meant to protect (it might have been popped in between)
//   * a thread that pops a node does not delete it; it RETIRES it (puts it on its own
//     retire list). When the list is long enough it SCANS: any retired node that no
//     thread's slot currently holds is safe to delete; the rest stay on the list.
//
// What you write: protect(), clear_protection(), retire(), scan() and the pop() changes
// (marked TODO). The slot registry, orphan hand-off and the harness are provided.
// As given, nothing is freed, so the harness reports LEAK. Your goal: no LEAK, still OK,
// and clean under TSan and ASan.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

constexpr std::size_t kCacheLine = 128;
constexpr int kMaxThreads = 32;

// ---- Node with allocation counters (the harness uses them to detect leaks) ---
std::atomic<long long> g_nodes_created{0};
std::atomic<long long> g_nodes_destroyed{0};

struct Node {
    int value;
    Node* next;
    Node() : value(0), next(nullptr) { g_nodes_created.fetch_add(1, std::memory_order_relaxed); }
    ~Node() { g_nodes_destroyed.fetch_add(1, std::memory_order_relaxed); }
};

// ---- Hazard slot registry (provided) ----------------------------------------
// One padded slot per thread (adjacent slots would false-share). A thread claims a slot
// the first time it touches the stack and gives it back when it exits.
struct alignas(kCacheLine) HazardSlot {
    std::atomic<Node*> hazard{nullptr};  // the node this thread is currently protecting
    std::atomic<bool> in_use{false};
};
HazardSlot g_slots[kMaxThreads];

// Retired nodes handed over by threads that exited while some of them were still protected.
std::mutex g_orphans_mutex;
std::vector<Node*> g_orphans;

struct ThreadState {
    int slot = -1;
    std::vector<Node*> retired;  // this thread's retire list

    ThreadState() {
        for (int i = 0; i < kMaxThreads; ++i) {
            bool expected = false;
            if (g_slots[i].in_use.compare_exchange_strong(expected, true)) {
                slot = i;
                return;
            }
        }
        std::fprintf(stderr, "out of hazard slots (kMaxThreads=%d)\n", kMaxThreads);
        std::abort();
    }
    ~ThreadState() {
        g_slots[slot].hazard.store(nullptr, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_orphans_mutex);
            for (Node* n : retired) g_orphans.push_back(n);
        }
        g_slots[slot].in_use.store(false, std::memory_order_release);
    }
};

ThreadState& this_thread() {
    thread_local ThreadState state;
    return state;
}

// ---- The stack --------------------------------------------------------------
class HazardStack {
public:
    ~HazardStack() {
        // Everything has finished by now, so nothing needs protecting any more:
        // free what is still in the stack, the caller's retire list and the orphans.
        for (Node* n = head_.load(); n;) {
            Node* next = n->next;
            delete n;
            n = next;
        }
        auto& mine = this_thread().retired;
        for (Node* n : mine) delete n;
        mine.clear();
        std::lock_guard<std::mutex> lock(g_orphans_mutex);
        for (Node* n : g_orphans) delete n;
        g_orphans.clear();
    }

    void push(int value) {  // unchanged from problem 4
        Node* n = new Node;
        n->value = value;
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        }
    }

    bool pop(int& out) {
        // TODO: protect `head_` before reading old->next / old->value, clear the
        // protection when done, and retire() the node instead of leaking it.
        Node* old;
        while (true) {
            old = protect(head_);
            if (old == nullptr) {
                clear_protection();
                return false;
            }
            if (head_.compare_exchange_weak(old, old->next, std::memory_order_relaxed,
                                            std::memory_order_relaxed))
                break;
        }
        out = old->value;
        clear_protection();
        retire(old);
        return true;
    }

private:
    // Publish a node in this thread's hazard slot and return it. It is only protected once
    // it is published AND confirmed to still be what `src` points to. Returns nullptr
    // if `src` is null.
    Node* protect(std::atomic<Node*>& src) {
        auto& slot = g_slots[this_thread().slot].hazard;
        while (true) {
            Node* p = src.load(std::memory_order_acquire);
            if (p == nullptr) {
                slot.store(nullptr, std::memory_order_release);
                return nullptr;
            }
            slot.store(p, std::memory_order_seq_cst);
            if (src.load(std::memory_order_seq_cst) == p) return p;
        }
    }

    void clear_protection() {
        g_slots[this_thread().slot].hazard.store(nullptr, std::memory_order_release);
    }

    // Take ownership of a node that has been unlinked from the stack. It may still be
    // read by other threads, so do not delete it blindly.
    void retire(Node* node) {
        auto& list = this_thread().retired;
        list.push_back(node);
        if (list.size() >= 2 * kMaxThreads) scan();
    }

    // Delete every retired node that no thread currently has in its hazard slot.
    void scan() {
        std::vector<Node*> protected_nodes;
        for (int i = 0; i < kMaxThreads; ++i) {
            Node* h = g_slots[i].hazard.load(std::memory_order_seq_cst);
            if (h) protected_nodes.push_back(h);
        }

        auto& list = this_thread().retired;
        std::vector<Node*> still_in_use;
        for (Node* n: list) {
            bool in_use = std::find(protected_nodes.begin(), protected_nodes.end(), n)
                          != protected_nodes.end();
            if (in_use) still_in_use.push_back(n);
            else delete n;
        }

        list = std::move(still_in_use);
    }

    std::atomic<Node*> head_{nullptr};
};

// ---- Harness ----------------------------------------------------------------
constexpr int kItemsPerProducer = 200'000;
constexpr int kTimeoutSeconds = 60;

bool sanity_check() {
    HazardStack s;
    int v = -1;
    if (s.pop(v)) {
        std::printf("sanity FAIL: pop on empty stack returned true\n");
        return false;
    }
    for (int i = 0; i < 10; ++i) s.push(i);
    for (int i = 9; i >= 0; --i) {
        if (!s.pop(v) || v != i) {
            std::printf("sanity FAIL: expected %d, got %d\n", i, v);
            return false;
        }
    }
    if (s.pop(v)) {
        std::printf("sanity FAIL: stack should be empty again\n");
        return false;
    }
    std::printf("sanity OK (LIFO order, empty pop)\n");
    return true;
}

struct Result {
    double seconds;
    long long lost, duplicated, out_of_range;
    long long created, destroyed;
    bool values_ok() const { return !lost && !duplicated && !out_of_range; }
    bool leaked() const { return created != destroyed; }
};

std::atomic<int> g_run_id{0};

void start_watchdog(int producers, int consumers) {
    int id = ++g_run_id;
    std::thread([id, producers, consumers] {
        std::this_thread::sleep_for(std::chrono::seconds(kTimeoutSeconds));
        if (g_run_id.load() != id) return;
        std::fprintf(stderr, "TIMEOUT after %ds (%dP/%dC): hang or lost element\n",
                     kTimeoutSeconds, producers, consumers);
        std::_Exit(1);
    }).detach();
}

inline void spin() { asm volatile("" ::: "memory"); }

Result run(int producers, int consumers) {
    const long long total = static_cast<long long>(producers) * kItemsPerProducer;
    std::atomic<long long> consumed{0};
    std::vector<std::vector<int>> taken(consumers);
    for (auto& v : taken) v.reserve(total / consumers + 1024);

    const long long created_before = g_nodes_created.load();
    const long long destroyed_before = g_nodes_destroyed.load();
    double seconds;
    {
        HazardStack stack;
        std::vector<std::thread> threads;
        auto start = std::chrono::steady_clock::now();
        for (int p = 0; p < producers; ++p)
            threads.emplace_back([&stack, p] {
                for (int i = 0; i < kItemsPerProducer; ++i) stack.push(p * kItemsPerProducer + i);
            });
        for (int c = 0; c < consumers; ++c)
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
        for (auto& t : threads) t.join();
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }  // stack destroyed here: frees what is left

    Result r{seconds, 0, 0, 0, g_nodes_created.load() - created_before,
             g_nodes_destroyed.load() - destroyed_before};
    std::vector<std::uint8_t> seen(total, 0);
    for (const auto& list : taken)
        for (int value : list) {
            if (value < 0 || value >= total) {
                ++r.out_of_range;
                continue;
            }
            if (seen[value]++) ++r.duplicated;
        }
    for (long long v = 0; v < total; ++v)
        if (!seen[v]) ++r.lost;
    return r;
}

int main() {
    if (!sanity_check()) return 1;
    for (auto [p, c] : {std::pair{1, 1}, std::pair{2, 2}, std::pair{4, 4}, std::pair{8, 8}}) {
        start_watchdog(p, c);
        Result r = run(p, c);
        long long total = static_cast<long long>(p) * kItemsPerProducer;
        std::printf("%dP/%dC time=%.4fs  items/s=%.3e  values:%s  nodes:%s "
                    "(lost=%lld dup=%lld range=%lld created=%lld destroyed=%lld)\n",
                    p, c, r.seconds, total / r.seconds, r.values_ok() ? "OK" : "WRONG",
                    r.leaked() ? "LEAK" : "OK", r.lost, r.duplicated, r.out_of_range,
                    r.created, r.destroyed);
        std::fflush(stdout);
    }
}
