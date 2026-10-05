# Problem 2: Thread-safe Queue — Notes

Code: [main.cpp](main.cpp). Build and run:

```
cmake -S . -B build && cmake --build build && ./build/concurrent_queue
cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan && ./build-tsan/concurrent_queue
```

Task: wrap `std::queue<int>` as `push(int)` / `bool try_pop(int&)`. No lost elements, no duplicates, no deadlock, all threads terminate. Tested with 1P/1C, 2P/2C and 4P/4C (producers/consumers), 500,000 items per producer.

## Implementation

```cpp
void push(int v)        { scoped_lock lock(mtx); queue_.push(v); }
bool try_pop(int& out)  { scoped_lock lock(mtx);
                          if (queue_.empty()) return false;
                          out = queue_.front(); queue_.pop(); return true; }
```

## Harness

Each producer pushes unique values `p * N + i`, so a value encodes who made it and in what order. Consumers pop until the total popped equals the total pushed. Afterwards it checks:

| Check | Catches |
|---|---|
| lost | a value was never popped |
| duplicated | a value was popped twice |
| out of range | garbage value |
| order violations | one consumer saw a producer's items out of order (FIFO broken) |

A watchdog kills the run after 15 s and prints TIMEOUT. A deadlock or lost element otherwise shows up as consumers spinning forever.

## Result (Release, 1 run)

| config | items/s |
|---|---|
| 1P/1C | 2.3e7 |
| 2P/2C | 1.9e7 |
| 4P/4C | 2.3e7 |

All OK, and TSan is clean. Throughput is flat in the thread count: every thread contends for one lock, the same lesson as Problem 1.

## Key points

**Check-then-act must be under one lock.** `try_pop` has to do "is it empty?", "read front" and "pop" as one critical section. If you unlock between the check and the pop, another consumer can empty the queue in between, and `front()` on an empty queue is undefined behavior (TOCTOU: time of check, time of use). A pusher can't break the check, since it only makes the queue less empty. A second consumer can.

**The lock doesn't "block pushes when empty".** When `try_pop` returns false the `scoped_lock` destructor releases the mutex immediately, so pushes proceed. The lock only guarantees nobody else touches the queue while you hold it.

**Spinning consumers.** `try_pop` is non-blocking, so consumers on an empty queue loop and take the lock repeatedly, wasting CPU. The standard fix is a `condition_variable` with a blocking `pop()` (needs `unique_lock`). Not required here.

## Can it be lock-free? (Discussion, not implemented)

You can't make `std::queue` atomic (not trivially copyable). The queue has to be redesigned so its internals are synchronized with atomics. Two families:

1. **Atomic indices into a fixed array (ring buffer).** Shared `head`, `tail` and the array. This is Problem 3.
2. **Atomic pointers in a linked list (Michael-Scott).** Needs `compare_exchange` and has the memory-reclamation problem (Problem 4).

**Ordering rule:** release publishes, acquire consumes. The producer writes the element, then updates `tail` with release. The consumer reads `tail` with acquire, so if it sees the new `tail` it sees the element. The consumer frees a slot by updating `head` with release, and the producer reads `head` with acquire before overwriting.

**Why SPSC is easy and MPMC is hard.**
- SPSC: each index has exactly one writer, so plain atomic load/store with the right orderings is enough, with no read-modify-write.
- MPMC: two producers can read the same `tail` and claim the same slot, so you need `compare_exchange` or `fetch_add`. Then a second problem appears: a producer has claimed a slot but hasn't finished writing it, and a consumer sees `tail` pass it and reads garbage. Fixing that needs per-slot state (sequence numbers, as in Vyukov's bounded MPMC queue).

Questions to carry into Problem 3:
- Who writes `head` and who writes `tail`? Can a thread read its own index relaxed?
- How do you tell full from empty when `head == tail`?
- What comes first, the element write or the index update?

## Takeaways

1. Any check followed by an action that depends on it must be one critical section.
2. A mutex queue is correct and simple, but throughput doesn't scale with threads.
3. A correctness harness needs unique, self-describing values so you can detect lost, duplicated and reordered items.
4. Lock-free queues are easiest when each shared variable has a single writer. Start with SPSC.
