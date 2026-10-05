// Does backing off after a failed compare_exchange help the lock-free stack under contention?
// Nodes are preallocated so malloc is out of the picture. Backoff: after each failed CAS,
// spin for a random number of iterations up to a limit that doubles per failure (capped).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <functional>
#include <random>
#include <thread>
#include <vector>

constexpr int kItems = 200'000;
struct Node { int value; Node* next; };
std::atomic<long long> g_fails{0};

inline void pause_for(unsigned n) {
    for (unsigned i = 0; i < n; ++i) asm volatile("yield" ::: "memory");  // ARM spin hint
}

// One random generator per thread. (Seeding a new one per operation with std::random_device
// is a system call and dominated the first version of this experiment.)
std::minstd_rand& thread_rng() {
    thread_local std::minstd_rand rng{static_cast<unsigned>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()))};
    return rng;
}

struct Backoff {
    unsigned limit = 1, max;
    explicit Backoff(unsigned m) : max(m) {}
    void operator()() {
        if (max == 0) return;
        pause_for(thread_rng()() % limit + 1);
        limit = std::min(limit * 2, max);
    }
};

template <unsigned MaxBackoff>
struct LF {
    std::atomic<Node*> head{nullptr};
    void push(Node* n) {
        Backoff b(MaxBackoff);
        long long f = 0;
        n->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(n->next, n, std::memory_order_release,
                                           std::memory_order_relaxed)) { ++f; b(); }
        if (f) g_fails.fetch_add(f, std::memory_order_relaxed);
    }
    bool pop(int& out) {
        Backoff b(MaxBackoff);
        long long f = 0;
        Node* old;
        while (true) {
            old = head.load(std::memory_order_acquire);
            if (!old) break;
            if (head.compare_exchange_weak(old, old->next, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) break;
            ++f; b();
        }
        if (f) g_fails.fetch_add(f, std::memory_order_relaxed);
        if (!old) return false;
        out = old->value;
        return true;
    }
};

struct ML {
    std::mutex m; Node* head = nullptr;
    void push(Node* n) { std::scoped_lock l(m); n->next = head; head = n; }
    bool pop(int& out) { std::scoped_lock l(m); if (!head) return false; out = head->value; head = head->next; return true; }
};

template <class S> double run(int n) {
    S s; long long total = (long long)n * kItems; std::atomic<long long> consumed{0};
    std::vector<std::vector<Node>> pre(n);
    for (int p = 0; p < n; ++p) { pre[p].resize(kItems); for (int i = 0; i < kItems; ++i) pre[p][i].value = p * kItems + i; }
    std::vector<std::thread> t;
    auto st = std::chrono::steady_clock::now();
    for (int p = 0; p < n; ++p) t.emplace_back([&, p] { for (int i = 0; i < kItems; ++i) s.push(&pre[p][i]); });
    for (int c = 0; c < n; ++c) t.emplace_back([&] {
        int v;
        while (consumed.load(std::memory_order_relaxed) < total)
            if (s.pop(v)) consumed.fetch_add(1, std::memory_order_relaxed); else asm volatile("" ::: "memory");
    });
    for (auto& x : t) x.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count();
}

template <class S> void row(const char* name, int n) {
    std::vector<double> v; g_fails = 0;
    for (int i = 0; i < 5; ++i) v.push_back(run<S>(n));
    std::sort(v.begin(), v.end());
    double ops = 2.0 * n * kItems * 5;  // pushes + pops over 5 runs
    std::printf("%dP/%dC  %-22s %-10.1f %.2f\n", n, n, name, n * kItems / v[2] / 1e6, g_fails.load() / ops);
}

int main() {
    std::printf("P/C    structure              M items/s  failed CAS per op\n");
    for (int n : {1, 2, 4, 8}) {
        row<LF<0>>("lock-free, no backoff", n);
        row<LF<64>>("lock-free, backoff 64", n);
        row<LF<1024>>("lock-free, backoff 1024", n);
        row<ML>("mutex linked list", n);
    }
}
