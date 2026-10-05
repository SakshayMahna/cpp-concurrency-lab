#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
constexpr std::size_t kCL = 128;
template <typename T, std::size_t N> class Q {
public:
  bool try_push(const T& v){ auto t=tail.load(std::memory_order_relaxed);
    if(t-cached_head==N){ cached_head=head.load(std::memory_order_acquire); if(t-cached_head==N) return false; }
    q[t%N]=v; tail.store(t+1,std::memory_order_release); return true; }
  bool try_pop(T& o){ auto h=head.load(std::memory_order_relaxed);
    if(h==cached_tail){ cached_tail=tail.load(std::memory_order_acquire); if(h==cached_tail) return false; }
    o=q[h%N]; head.store(h+1,std::memory_order_release); return true; }
private:
  T q[N];
  alignas(kCL) std::atomic<std::size_t> head{0}; std::size_t cached_tail{0};
  alignas(kCL) std::atomic<std::size_t> tail{0}; std::size_t cached_head{0};
};
constexpr long long kItems = 10'000'000;
template <std::size_t N> double run(){
  auto* q = new Q<int,N>;
  auto st=std::chrono::steady_clock::now();
  std::thread p([&]{ for(long long i=0;i<kItems;i++) while(!q->try_push((int)i)) asm volatile("":::"memory"); });
  std::thread c([&]{ int v; for(long long i=0;i<kItems;i++) while(!q->try_pop(v)) asm volatile("":::"memory"); });
  p.join(); c.join();
  double s=std::chrono::duration<double>(std::chrono::steady_clock::now()-st).count();
  delete q; return s;
}
template <std::size_t N> void row(){
  std::vector<double> v; for(int i=0;i<5;i++) v.push_back(run<N>()); std::sort(v.begin(),v.end());
  std::printf("%-8zu %-10zu %.1f\n", N, N*sizeof(int)/1024, kItems/v[2]/1e6);
}
int main(){
  std::printf("capacity ring KB    M items/s (median of 5)\n");
  row<64>(); row<256>(); row<1024>(); row<2048>(); row<4096>(); row<8192>(); row<16384>();
  row<32768>(); row<65536>(); row<131072>(); row<524288>();
}
