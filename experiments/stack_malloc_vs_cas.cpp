#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stack>
#include <thread>
#include <vector>
constexpr int kItems = 200'000;
struct Node { int value; Node* next; };
std::atomic<long long> g_cas_fail{0};

// Lock-free stack. Node supplied by caller (so allocation can be done outside).
struct LF {
  std::atomic<Node*> head{nullptr};
  void push_node(Node* n){
    n->next = head.load(std::memory_order_relaxed);
    long long fails=0;
    while(!head.compare_exchange_weak(n->next,n,std::memory_order_release,std::memory_order_relaxed)) ++fails;
    if(fails) g_cas_fail.fetch_add(fails,std::memory_order_relaxed);
  }
  bool pop(int& out){
    Node* old; long long fails=0;
    while(true){
      old=head.load(std::memory_order_acquire);
      if(!old) return false;
      if(head.compare_exchange_weak(old,old->next,std::memory_order_relaxed,std::memory_order_relaxed)) break;
      ++fails;
    }
    if(fails) g_cas_fail.fetch_add(fails,std::memory_order_relaxed);
    out=old->value; return true;
  }
};
// Mutex-guarded linked list, same node supply.
struct ML {
  std::mutex m; Node* head=nullptr;
  void push_node(Node* n){ std::scoped_lock l(m); n->next=head; head=n; }
  bool pop(int& out){ std::scoped_lock l(m); if(!head) return false; out=head->value; head=head->next; return true; }
};
// Mutex std::stack (baseline of the harness).
struct MS {
  std::mutex m; std::stack<int> s;
  void push_node(Node* n){ std::scoped_lock l(m); s.push(n->value); }
  bool pop(int& out){ std::scoped_lock l(m); if(s.empty()) return false; out=s.top(); s.pop(); return true; }
};

// mode 0: allocate with new inside the timed push; mode 1: nodes preallocated before timing.
template <class S> double run(int P,int C,int mode){
  S s; long long total=(long long)P*kItems; std::atomic<long long> consumed{0};
  std::vector<std::vector<Node>> pre(P);
  if(mode==1) for(int p=0;p<P;p++){ pre[p].resize(kItems); for(int i=0;i<kItems;i++) pre[p][i].value=p*kItems+i; }
  std::vector<std::thread> t;
  auto st=std::chrono::steady_clock::now();
  for(int p=0;p<P;p++) t.emplace_back([&,p]{
    for(int i=0;i<kItems;i++){
      Node* n = mode==1 ? &pre[p][i] : new Node{p*kItems+i,nullptr};
      s.push_node(n);
    }});
  for(int c=0;c<C;c++) t.emplace_back([&]{
    int v; while(consumed.load(std::memory_order_relaxed)<total){ if(s.pop(v)) consumed.fetch_add(1,std::memory_order_relaxed); else asm volatile("":::"memory"); }});
  for(auto&x:t) x.join();
  return std::chrono::duration<double>(std::chrono::steady_clock::now()-st).count();
}
template <class S> double med(int P,int C,int mode){ std::vector<double> v; for(int i=0;i<5;i++) v.push_back(run<S>(P,C,mode)); std::sort(v.begin(),v.end()); return v[2]; }
int main(){
  std::printf("%-6s | %-26s | mode0 (new in push) M items/s | mode1 (preallocated) M items/s\n","P/C","structure");
  for(int n:{1,2,4,8}){
    long long tot=(long long)n*kItems;
    auto row=[&](const char* name,double a,double b){ std::printf("%dP/%dC  | %-26s | %-27.1f | %.1f\n",n,n,name,tot/a/1e6,tot/b/1e6); };
    row("lock-free (CAS)", med<LF>(n,n,0), med<LF>(n,n,1));
    row("mutex linked list", med<ML>(n,n,0), med<ML>(n,n,1));
    row("mutex std::stack", med<MS>(n,n,0), med<MS>(n,n,1));
  }
  g_cas_fail=0; run<LF>(8,8,1);
  std::printf("\nlock-free 8P/8C preallocated: %lld failed CAS for %d pushes+%d pops (%.2f failures per op)\n",
     g_cas_fail.load(), 8*kItems, 8*kItems, g_cas_fail.load()/(2.0*8*kItems));
}
