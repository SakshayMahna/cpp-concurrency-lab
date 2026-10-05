# Problem 4: Lock-free Stack (Treiber stack) — Notes

Code: [main.cpp](main.cpp). Build and run:

```
cmake -S . -B build && cmake --build build && ./build/lockfree_stack
cmake -S . -B build-tsan -DSANITIZE=thread  && cmake --build build-tsan --target lockfree_stack && ./build-tsan/lockfree_stack
cmake -S . -B build-asan -DSANITIZE=address && cmake --build build-asan --target lockfree_stack && ./build-asan/lockfree_stack
```

Task: a stack where the top is a `std::atomic<Node*>`. `void push(int)`, `bool pop(int&)`. Tested with a single-threaded sanity check (LIFO, pop on empty) and a stress test at 1P/1C, 2P/2C, 4P/4C, 8P/8C (producers/consumers, 500,000 items each) that checks every value is popped exactly once. Popped nodes are leaked on purpose (see reclamation below).

Machine: Apple M3 Pro. Single-session numbers, indicative only.

## Algorithm

```cpp
void push(int value) {
    Node* n = new Node; n->value = value;
    n->next = head.load(relaxed);
    while (!head.compare_exchange_weak(n->next, n,
               release, relaxed));            // commit: top := n, only if top is still what I saw
}

bool pop(int& out) {
    Node* old;
    while (true) {
        old = head.load(acquire);
        if (!old) return false;
        if (head.compare_exchange_weak(old, old->next, relaxed, relaxed)) break;
    }
    out = old->value;                          // read after the commit
    return true;
}
```

## `compare_exchange`

`head.compare_exchange_weak(expected, desired)` is one indivisible step: if `head == expected`, set it to `desired` and return true. Otherwise leave it alone, copy the actual current value into `expected`, and return false. "Update this, but only if nobody changed it since I looked." The pattern is look, prepare, commit, and retry on failure. No lock is held at any point.

- `push`: a failed exchange writes the new top into `n->next`, so the retry already has the right `next`.
- `weak` vs `strong`: weak may fail spuriously (on CPUs that build CAS from load-linked/store-conditional) but is cheaper per attempt. In a retry loop use weak, because you retry on failure anyway. Use strong when there is no loop or a failure is expensive to handle. On Apple silicon there is a real CAS instruction, so they behave almost the same, but weak keeps the code portable.

## Memory ordering

The problem: each node's `value` and `next` are written by one thread and read by another, and the only link between them is the `head` pointer.

- `release` on the successful push exchange: everything written before it (the node's fields) becomes visible to anyone who sees this pointer.
- `acquire` on the pop's load of `head`: pairs with that release, so if the popper sees the node it also sees its `value` and `next`.
- The pop's exchange only decides who wins the race to swing `head`. The acquire load already gave visibility, so both of its orders can be relaxed.
- A failure order must be a load order (`relaxed`/`acquire`/`seq_cst`). A failed exchange writes nothing, so `release` as the failure order is invalid. The compiler warned about that in an earlier version.

| Where | Order | Reason |
|---|---|---|
| push exchange, success | release | publish the node's `value` and `next` |
| push exchange, failure | relaxed | nothing published, just retry |
| pop `head.load` | acquire | pairs with the push's release |
| pop exchange (both) | relaxed | the acquire load already gave visibility |

Wrong orders usually pass the stress test and fail rarely or on other hardware, which is why TSan matters.

## Bugs hit along the way

1. **Single-threaded `delete head;` after `head = head->next`** deleted the next node (still in the stack) and leaked the popped one. ASan reported heap-use-after-free, though the sanity check printed OK by luck. Fix: save the popped node in a local, then advance `head`.
2. **First atomic `pop` read `out` before the retry loop.** After a failed exchange `old` changes to another node, but `out` kept the first node's value. Result: equal numbers of lost and duplicated items. It could also dereference null if the stack emptied between attempts. Fix: do look, check and commit all inside the loop, and read the value after the commit.
3. **Variable shadowing:** `Node* old = ...` inside the loop declared a second variable, so the one used after the loop was uninitialized. The compiler warning (`-Wuninitialized`) pointed straight at it.
4. **Reversed memory orders** on the pop exchange (relaxed success, release failure): invalid, and the compiler warned.

## Results (items/s, Release)

| threads | mutex `std::stack` | lock-free |
|---|---|---|
| 1P/1C | 5.3e7 | 2.5e7–4.5e7 |
| 2P/2C | 2.6e7 | 6.5e6 |
| 4P/4C | 2.3e7 | 2.4e6 |
| 8P/8C | 2.1e7 | 1.5e6 |

TSan and ASan: clean. Lock-free does not mean faster. Lock-free gives a progress guarantee (a stalled thread can't block the others), not speed.

Why it is slower, measured (details in [RESULTS.md](../RESULTS.md)):
- `malloc` is a real cost but not the main one: with nodes preallocated the lock-free stack at 8P/8C only goes from 1.4 to 1.5 million items/s, while a mutex-guarded linked list goes from 7.4 to 22.7.
- The dominant cost is CAS contention on the single `head` pointer: at 8P/8C there were 7.2 failed `compare_exchange` attempts per operation. Each failure is a wasted attempt plus a cache-line transfer. The mutex serializes the same work with far less cache-line traffic.
- Randomized exponential backoff after a failed CAS makes the lock-free stack about 6x faster at 2P/2C (3.4x faster than the mutex there), but at 4P/4C and 8P/8C it stays about 10x slower than the mutex.

## Does cache-line alignment help here?

Tried `alignas(128)` on `head` (scratch copy, 3 runs each): no improvement, slightly worse at 1P/1C (about 3.4e7 vs 4.5e7) and within noise at the other thread counts.

Reason: padding fixes **false sharing**, which is two threads writing *different* variables in one cache line. In this stack the contention is **true sharing**: every thread writes the same variable, `head`. Padding can't help that; only touching `head` less often can. Alignment would matter in a variant with several hot variables written by different threads, such as the per-thread slots of hazard pointers (next problem).

## Reclamation: why you can't just `delete` popped nodes

Experiment (scratch copy with `delete old;` after the commit): ASan reports `heap-use-after-free` on the read of `old->next`. In the Release build 1P/1C and 2P/2C passed by luck, then every run crashed with a segmentation fault at 4P/4C.

Why it breaks:
- **Use-after-free:** thread A loads `old` (node X). Thread B pops X and deletes it. Thread A then reads `old->next` from freed memory. A's exchange would fail, but the bad read has already happened.
- **ABA:** after X is freed, the allocator can reuse the address for a new node. Thread A's exchange compares only the pointer, sees "same as before" and succeeds, but it writes a stale `next` it computed earlier, corrupting the stack.

Safe to free only when no other thread can still hold a pointer to the node:

| Approach | Idea |
|---|---|
| Never free (this version) | Leak, or never reuse addresses. Safe, but memory grows without bound. |
| Hazard pointers | Before dereferencing, a thread publishes "I'm using X" in a per-thread slot. To free X, check all slots. If any lists X, keep X on a retire list and retry later. |
| Epoch-based reclamation | Threads announce an epoch. A retired node is freed once all threads have moved past the epoch it was retired in. |
| Reference counting | A count tracks holders and the last one frees. The count itself must be updated safely (`std::atomic<std::shared_ptr>` may use a lock internally). |
| Tagged pointers | Store a version counter beside the pointer so a reused address no longer compares equal. Fixes ABA, not reading freed memory. |

C++26 adds `std::hazard_pointer` and `std::rcu`.

## Takeaways

1. A lock-free retry loop does look, check and commit entirely inside the loop. Anything read before the loop can be stale after a failed exchange.
2. `compare_exchange` failure writes the current value into `expected`. Use it, don't re-read.
3. Release publishes data, acquire consumes it. Failure orders must be load orders.
4. Lock-free is about progress guarantees, not raw speed. A single hot pointer is true sharing, and padding doesn't fix that.
5. Memory reclamation is the hard part of lock-free structures. Passing the stress test with `delete` is luck. Use ASan.
6. Read compiler warnings. Two of the bugs above were reported by them.
