# Problem 3: Lock-free SPSC Queue — Notes

Code: [main.cpp](main.cpp). Build and run:

```
cmake -S . -B build && cmake --build build && ./build/spsc_queue
cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan && ./build-tsan/spsc_queue
```

Task: fixed-capacity ring buffer for exactly one producer and one consumer. `bool try_push(const T&)`, `bool try_pop(T&)`. No mutex, no allocation after construction. Capacities tested: 2, 16, 1024, 65536 (tiny ones force constant wrap-around and full/empty transitions). The consumer must receive exactly 0, 1, 2, ... for 1,000,000 items.

Machine: Apple M3 Pro, 128-byte cache lines. Single-session numbers, indicative only.

## Design

- Storage is `T queue[N]` inside the object (`N` is a compile-time template value), so there is no dynamic allocation.
- `head` (next slot to read) and `tail` (next slot to write) are unbounded counters. The slot is `index % N`.
- Empty: `head == tail`. Full: `tail - head == N`. No wasted slot, and unsigned subtraction stays correct on wrap-around.
- Single writer per index: only the producer writes `tail`, only the consumer writes `head`.

```cpp
bool try_push(const T& value) {
    const auto t = tail.load(relaxed);                 // own index
    if (t - cached_head == N) {                        // looks full with the old head
        cached_head = head.load(acquire);              // refresh the other thread's index
        if (t - cached_head == N) return false;
    }
    queue[t % N] = value;                              // write the element first...
    tail.store(t + 1, release);                        // ...then publish it
    return true;
}
// try_pop is the mirror image (head / cached_tail).
```

## Memory ordering

- A thread reading **its own** index needs no ordering (relaxed). No other thread writes it, and a thread always sees its own latest write.
- A thread reading the **other** thread's index uses acquire. The owner publishes with release, after the data it guards. If the consumer sees the new `tail`, it sees the element written before it.
- Symmetrically, the consumer frees a slot with a release store to `head`, and the producer's acquire load of `head` makes sure it doesn't overwrite a slot that is still being read.
- Single writer means a plain `store` is enough. `fetch_add` (a read-modify-write) is only needed when several threads write the same variable.
- Acquire/release controls **visibility** of data between threads. It does not provide **mutual exclusion**. The queue is correct only with exactly one producer thread and one consumer thread. With two producers, both could read the same `tail`, write the same slot, and lose an item. Multiple producers or consumers need atomic claiming (`compare_exchange`/`fetch_add`) and per-slot state. That is the MPMC problem.

## The plain-index version fails

Before going atomic, the same code with plain `size_t` indices gave WRONG results at capacities 2 and 16 (mismatches) and passed at 1024 and 65536. Large capacities pass by luck, because the two threads are far apart and rarely collide. It is still a data race (undefined behavior). The small capacities keep both threads at the edge of full or empty, which is why the harness uses them.

## Results

Plain atomic indices with `fetch_add`, then each improvement (spsc, items/s, Release):

| capacity | atomic + fetch_add | + store, padded head/tail | + cached other index |
|---|---|---|---|
| 2 | 1.7e7 | 2.5e7 | 2.2e7 |
| 16 | 1.9e7 | 3.3e7 | 2.9e7 |
| 1024 | 2.0e7 | 3.7e7 | 0.8e8–1.9e8 |
| 65536 | 8.2e7 | 2.6e7 | 2.5e7 |

TSan: clean at all capacities.

Caveats:
- The bounded mutex baseline in the harness was erratic on this machine (the same configuration ranged from about 0.02 s to over 10 s between sessions, and one capacity-2 run timed out). Don't draw conclusions from that column. Compare in one sitting, several times.
- Capacity 2 and 16 are stress tests, not speed tests. The queue is nearly always full or empty, so the cached index is refreshed constantly.
- Capacity 65536 stays at about 2.5e7 after the optimizations. Measured (see [RESULTS.md](../RESULTS.md)): throughput falls steadily as the ring grows and follows the ring's size in bytes, not the slot count, so it is a memory-footprint effect, not the index logic. At large sizes the ring is nearly always empty and the producer never waits: the producer is the bottleneck. The earlier guess ("ring larger than L1") is not supported; the exact cause needs hardware cache-miss counters.
- 1M items take only tens of milliseconds per run, so timing noise is large. A bigger `kItems` would be steadier.

## Cache lines and why the changes helped

A core doesn't fetch single bytes. Memory moves in **cache lines** (64 bytes on most x86, 128 on Apple silicon). A core keeps its own copies of lines in its cache. The coherence protocol keeps copies consistent: before a core **writes** a line, it must be the only one holding it, so every other core's copy is invalidated. If another core then reads or writes that line, the line has to be transferred across the chip, which takes tens of nanoseconds, versus about a nanosecond for a hit in the local cache.

Consequences for this queue:

1. **False sharing between `head` and `tail`.** Adjacent in memory means one line. The producer writes `tail` and the consumer writes `head`, so the single line ping-pongs between the cores on every push and pop, although neither thread touches the other's variable. `alignas(kCacheLine)` gives each its own line. Each core then keeps its own index's line exclusively.
2. **`fetch_add` vs `store`.** An atomic read-modify-write is a heavier instruction than a plain release store. With one writer you don't need it.
3. **Cached opposite index.** Without the cache, each push reads `head` and each pop reads `tail`. Those lines were just written by the other core, so each operation can pull a line across the chip. With the cache, `head` is read only when the queue looks full, so one transfer covers many pushes. `head` only ever increases, so a stale cached value only underestimates free space, which is safe. The cached copies are plain variables on the same line as their owner's index, so they add no sharing.
4. **Slots can also false-share.** 32 ints fit in one 128-byte line, so the producer writing slot k and the consumer reading slot k-1 can hit the same line. Letting the producer run ahead (made possible by the cache) spreads the threads apart.

Rule of thumb: the cost of a concurrent data structure is mostly the number of cache-line transfers per operation. The goal is to touch the other thread's line as rarely as possible.

## Takeaways

1. One writer per variable removes the need for read-modify-write atomics and compare-and-swap loops.
2. Release publishes data, acquire consumes it. A thread reading its own variable needs neither.
3. Memory order gives visibility, not mutual exclusion. SPSC depends on the caller keeping to one producer and one consumer.
4. Passing on large capacities proves little. Use tiny capacities to force the edge cases, and run TSan.
5. Performance work on shared data is about cache lines: pad per-thread data, avoid reading the other thread's variables, and cache what you can.
6. Benchmark in the same session, several times, and don't over-read numbers from short runs.
