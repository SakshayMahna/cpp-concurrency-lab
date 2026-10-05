// Why does SPSC throughput fall as the ring grows? Measures how full the ring is when the
// consumer pops (tail - head). A ring that is usually near full means the producer runs far
// ahead, so the consumer reads lines the producer wrote long ago. Also tests whether a
// consumer that pauses briefly when the queue is empty (instead of re-reading tail at once)
// changes throughput.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

constexpr std::size_t kCL = 128;
template <typename T, std::size_t N> class Q {
public:
    bool try_push(const T& v) {
        auto t = tail.load(std::memory_order_relaxed);
        if (t - cached_head == N) { cached_head = head.load(std::memory_order_acquire); if (t - cached_head == N) return false; }
        q[t % N] = v; tail.store(t + 1, std::memory_order_release); return true;
    }
    // Same as try_pop, plus reports the occupancy seen by the consumer.
    bool try_pop(T& o, std::size_t& occupancy) {
        auto h = head.load(std::memory_order_relaxed);
        if (h == cached_tail) { cached_tail = tail.load(std::memory_order_acquire); if (h == cached_tail) return false; }
        occupancy = cached_tail - h;
        o = q[h % N]; head.store(h + 1, std::memory_order_release); return true;
    }
private:
    T q[N];
    alignas(kCL) std::atomic<std::size_t> head{0}; std::size_t cached_tail{0};
    alignas(kCL) std::atomic<std::size_t> tail{0}; std::size_t cached_head{0};
};

constexpr long long kItems = 10'000'000;

template <std::size_t N> void row(unsigned consumer_pause) {
    auto* q = new Q<int, N>;
    long long full_spins = 0, empty_spins = 0;
    double occ_sum = 0;
    auto st = std::chrono::steady_clock::now();
    std::thread p([&] { for (long long i = 0; i < kItems; ++i) while (!q->try_push((int)i)) ++full_spins; });
    std::thread c([&] {
        int v; std::size_t occ;
        for (long long i = 0; i < kItems; ++i) { while (!q->try_pop(v, occ)) {
            ++empty_spins;
            for (unsigned k = 0; k < consumer_pause; ++k) asm volatile("yield" ::: "memory");
        } occ_sum += occ; }
    });
    p.join(); c.join();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count();
    std::printf("%-6u %-8zu %-9.1f %-14.0f %-14.2f %-16lld %lld\n", consumer_pause, N, kItems / s / 1e6, occ_sum / kItems,
                occ_sum / kItems / N, full_spins, empty_spins);
    delete q;
}

int main() {
    // pause = number of spin hints the consumer executes after finding the queue empty.
    std::printf("pause  capacity M items/s avg occupancy  fraction full  producer waits   consumer waits\n");
    for (unsigned pause : {0u, 64u}) {
        row<256>(pause); row<1024>(pause); row<4096>(pause); row<16384>(pause); row<65536>(pause); row<262144>(pause);
    }
}
