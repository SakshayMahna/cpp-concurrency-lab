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
template <typename T, std::size_t N> double run(){
  auto* q = new Q<T,N>;
  auto st=std::chrono::steady_clock::now();
  std::thread p([&]{ for(long long i=0;i<kItems;i++) while(!q->try_push((T)i)) asm volatile("":::"memory"); });
  std::thread c([&]{ T v; for(long long i=0;i<kItems;i++) while(!q->try_pop(v)) asm volatile("":::"memory"); });
  p.join(); c.join();
  double s=std::chrono::duration<double>(std::chrono::steady_clock::now()-st).count();
  delete q; return s;
}
template <typename T, std::size_t N> void row(const char* tn){
  std::vector<double> v; for(int i=0;i<5;i++) v.push_back(run<T,N>()); std::sort(v.begin(),v.end());
  std::printf("%-6s %-8zu %-8zu %.1f\n", tn, N, N*sizeof(T)/1024, kItems/v[2]/1e6);
}
int main(){
  std::printf("type   slots    ring KB  M items/s\n");
  row<char,16384>("char"); row<int,4096>("int");
  row<char,65536>("char"); row<int,16384>("int");
  row<char,262144>("char"); row<int,65536>("int");
  row<char,1048576>("char"); row<int,262144>("int");
}
