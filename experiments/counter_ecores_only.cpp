// Is the atomic counter's jump at the 6th thread caused by mixing core types?
// Background QoS restricts threads to the efficiency cores on Apple silicon (6 E-cores on
// this machine). If the jump disappears when every thread is on the same core type, mixing
// performance and efficiency cores is the cause.
#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

constexpr int kIters = 1'000'000;

double run(int n, bool ecores_only) {
    std::atomic<int> c{0};
    std::vector<std::thread> t;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i)
        t.emplace_back([&] {
            if (ecores_only) pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
            for (int j = 0; j < kIters; ++j) c.fetch_add(1, std::memory_order_relaxed);
        });
    for (auto& x : t) x.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

double median(int n, bool e) {
    std::vector<double> v;
    for (int i = 0; i < 7; ++i) v.push_back(run(n, e));
    std::sort(v.begin(), v.end());
    return v[3] / (double(n) * kIters) * 1e9;
}

int main() {
    std::printf("threads  default ns/op  efficiency-cores-only ns/op\n");
    for (int n : {1, 2, 3, 4, 5, 6})
        std::printf("%-8d %-13.1f %.1f\n", n, median(n, false), median(n, true));
}
