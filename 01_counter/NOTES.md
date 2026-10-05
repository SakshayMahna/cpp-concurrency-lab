# Problem 1: Thread-safe Counter — Notes

Code: [main.cpp](main.cpp). Build and run:

```
cmake -S . -B build && cmake --build build && ./build/counter
cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan && ./build-tsan/counter
```

Task: N threads x 1,000,000 increments each, final value must be exactly N x 1,000,000.
Machine: Apple M3 Pro (5 performance + 6 efficiency cores, 128-byte cache lines). Single runs, so treat figures as indicative.

## Implementations

| Name | Idea |
|---|---|
| `MutexCounter` | `int` guarded by `std::mutex`, `scoped_lock` in `increment()` and `get()` |
| `AtomicCounter` | `std::atomic<int>`, `fetch_add(relaxed)` |
| `local` | each thread counts in a local variable, one `fetch_add` at the end |
| `packed` | one atomic slot per thread, slots adjacent in memory |
| `padded` | same, but each slot `alignas(128)` (own cache line) |

## C++ points learned

**`const std::mutex` can't be locked.** `lock()` is non-const. A `const` method like `get() const` needs the mutex to be `mutable`. A mutex is bookkeeping, not part of the counter's logical value. Don't use `mutable` to bypass `const` on real data.

**`lock_guard` vs `scoped_lock`.** Both are RAII (lock in the constructor, unlock in the destructor, safe on early return and exceptions). `lock_guard` takes exactly one mutex. `scoped_lock` (C++17) takes any number, locks them with deadlock avoidance, and deduces its template arguments. Default to `scoped_lock`. Use `unique_lock` when you need to unlock early or use a `condition_variable` (Problem 2).

**Make a race visible.** A plain `int` with no lock gives WRONG totals. TSan flags it. Both correct versions are TSan-clean.

## Memory ordering

- `fetch_add` is an atomic read-modify-write, so no increments are lost whatever the ordering. Relaxed only drops ordering relative to *other* memory.
- Acquire does not "wait for increments to finish". It pairs with a release store so that data written before the release is visible after the acquire. A pure counter publishes nothing, so there is nothing to pair with. Relaxed is enough.
- After `join()`, `get()` is safe even relaxed, because `join()` synchronizes-with the end of the thread.
- Calling `get()` mid-run: relaxed on both sides is still correct for the counter's own value. The result is a possibly stale snapshot, which no ordering can fix. Reads are monotonic per thread (coherence).

| `get()` is used for | `increment` | `get` |
|---|---|---|
| Just a number (stats, progress) | relaxed | relaxed |
| "Count reached N, so the data is ready" | release | acquire (must pair with release) |
| One global order across several atomics | seq_cst | seq_cst |

- `const int v = counter;` is an implicit `load(seq_cst)`. Correct but stronger than needed, and it hides intent. Write the order explicitly.
- Mutex `lock`/`unlock` already give acquire/release for the critical section, so it is correct in every case. That is part of what its extra cost buys.

## Benchmark results (ops/sec, Release)

| threads | mutex | atomic | local | packed | padded |
|---|---|---|---|---|---|
| 1 | 2.2e8 | 5.2e8 | 3.6e9 | 5.2e8 | 5.2e8 |
| 2 | 1.3e8 | 2.7e8 | 6.4e9 | 2.5e8 | 1.0e9 |
| 4 | 6.6e7 | 1.5e8 | 1.2e10 | 1.2e8 | 2.0e9 |
| 8 | 6.1e7 | 3.7e7 | 1.7e10 | 3.0e7 | 3.5e9 |
| 16 | 5.8e7 | 3.4e7 | 1.9e10 | 4.2e7 | 3.6e9 |

(mutex, atomic and local are from one run, packed and padded from a later one, so atomic differs slightly between them.)

## Explanations

**Why a shared counter doesn't scale (true sharing).** Every increment needs exclusive ownership of the cache line holding the counter. The line moves between cores (tens of ns each time). More threads means more contention for the same line, so the work is fully serialized and you pay transfer cost on top. Total throughput falls as threads are added.

**Why atomic beats mutex at low thread counts.** `fetch_add` is a single hardware instruction. The mutex does lock, increment, unlock: at least two atomic operations on the mutex word plus the counter write plus call overhead. With 1 thread there is no contention, so this pure instruction cost shows (about 2.5x).

**Where the atomic loses to the mutex (measured, see [RESULTS.md](../RESULTS.md)).** The atomic is fast up to 5 threads (2.2 to 7.4 ns per increment) and jumps to about 21-39 ns from 6 threads, while the mutex stays flat at about 17-19 ns. `fetch_add` compiles to one `ldadd` instruction here, so the jump is not retries. With every thread kept on the efficiency cores, the same jump happens at 3 threads, so the cause is contention once efficiency cores take part, not mixing core types. The mutex stays flat because the lock is unfair: the holder usually re-acquires it, so ownership changes only once every 60-100 increments and the counter's cache line rarely moves.

**Why `local` is ~400x faster.** Each thread's loop touches only its own register. The shared atomic is touched once per thread rather than millions of times. It scales with cores and flattens past the core count (11). Limitation: nobody can read an accurate total while it runs.

**Why `padded` scales and `packed` doesn't (false sharing).** A cache line (128 bytes here) is the unit of ownership between cores. When different threads write different variables in the same line, the hardware still bounces the line even though nothing is logically shared. `packed` performs about the same as the single shared atomic. `padded` gives each thread its own line and scales almost linearly (about 100x faster than `packed` at 8 threads, from layout alone). Unlike `local`, the total can be read at any time by summing the slots.

## Takeaways

1. True sharing (same variable): share less, by batching or sharding.
2. False sharing (different variables, same line): fix the layout with padding or alignment.
3. Atomics are cheaper than mutexes uncontended, but neither fixes contention. Design for less sharing.
4. Memory order only matters for *other* memory that the atomic's value is meant to signal. A pure counter can be relaxed.
5. Check microbenchmarks: without `asm volatile("" : "+r"(local))` the compiler folds the `local` loop to a constant and measures nothing.
6. Run benchmarks several times and look at the spread. Don't use TSan builds for timing.
