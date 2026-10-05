#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
constexpr int kIters = 1'000'000;
struct M { std::mutex m; int c = 0; void inc(){ std::scoped_lock l(m); ++c; } };
struct A { std::atomic<int> c{0}; void inc(){ c.fetch_add(1, std::memory_order_relaxed);} };
template <class C> double run(int n){
  C c; std::vector<std::thread> t;
  auto s=std::chrono::steady_clock::now();
  for(int i=0;i<n;i++) t.emplace_back([&]{for(int j=0;j<kIters;j++) c.inc();});
  for(auto&x:t) x.join();
  return std::chrono::duration<double>(std::chrono::steady_clock::now()-s).count();
}
template <class C> double median(int n){ std::vector<double> v; for(int i=0;i<7;i++) v.push_back(run<C>(n)); std::sort(v.begin(),v.end()); return v[3]; }
int main(){
  std::printf("threads  mutex ns/op  atomic ns/op   (ns per increment, total wall time / total ops)\n");
  for(int n: {1,2,3,4,5,6,7,8,10,11,12,16}){
    double m=median<M>(n), a=median<A>(n);
    double ops=double(n)*kIters;
    std::printf("%-8d %-12.1f %-12.1f\n", n, m/ops*1e9, a/ops*1e9);
  }
}
