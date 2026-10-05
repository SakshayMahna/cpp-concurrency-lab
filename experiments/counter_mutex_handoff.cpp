// Why doesn't the mutex counter degrade past 5 threads like the atomic does?
// Measures how often the lock changes hands (owner switches) and how often threads sleep
// (voluntary context switches). If the lock rarely changes hands, the counter's cache line
// stays on one core for long runs of increments.
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

constexpr int kIters = 1'000'000;

long voluntary_switches() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_nvcsw;
}

int main() {
    std::printf("threads  ns/op  owner-switches  increments/switch  sleeps(voluntary ctx switches)\n");
    for (int n : {1, 2, 4, 5, 6, 8, 11, 16}) {
        std::mutex m;
        long long counter = 0, switches = 0;
        int last_owner = -1;
        long before = voluntary_switches();
        std::vector<std::thread> t;
        auto start = std::chrono::steady_clock::now();
        for (int id = 0; id < n; ++id)
            t.emplace_back([&, id] {
                for (int i = 0; i < kIters; ++i) {
                    std::scoped_lock l(m);
                    if (last_owner != id) {
                        ++switches;
                        last_owner = id;
                    }
                    ++counter;
                }
            });
        for (auto& x : t) x.join();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        long sleeps = voluntary_switches() - before;
        std::printf("%-8d %-6.1f %-15lld %-18.0f %ld\n", n, s / counter * 1e9, switches,
                    double(counter) / switches, sleeps);
    }
}
