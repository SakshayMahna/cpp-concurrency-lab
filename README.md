# C++ Concurrency Lab

Small concurrency problems in C++20, worked through from a simple version to a lock-free one. Each problem has its own stress-test harness and notes, and gets checked with ThreadSanitizer and AddressSanitizer. Where the benchmark numbers looked surprising, there are follow-up experiments to explain them.

The aim is not just code that passes. For each structure, the questions are:
- Is it free of data races and deadlocks?
- Do the memory orders hold up?
- Do all threads make progress?
- How does it behave under contention, and *why*?

## Problems

| # | Problem | What it covers | Notes |
|---|---|---|---|
| 1 | [Thread-safe counter](01_counter/) | `std::mutex` vs `std::atomic`, memory order for a pure counter, true vs false sharing, thread-local batching, cache-line padding | [NOTES](01_counter/NOTES.md) |
| 2 | [Concurrent queue](02_concurrent_queue/) | Mutex-guarded `std::queue`, check-then-act races, what a lock-free queue would need | [NOTES](02_concurrent_queue/NOTES.md) |
| 3 | [Lock-free SPSC queue](03_spsc_queue/) | Ring buffer with one writer per index, acquire/release publishing, full vs empty, padding, cached indices | [NOTES](03_spsc_queue/NOTES.md) |
| 4 | [Lock-free stack](04_lockfree_stack/) | Treiber stack, `compare_exchange` retry loops, weak vs strong, why naive `delete` is unsafe (use-after-free, ABA) | [NOTES](04_lockfree_stack/NOTES.md) |
| 4b | [Hazard pointers](04b_hazard_pointers/) | Safe memory reclamation for the Treiber stack: publish and re-check, retire lists, scanning, where `seq_cst` is required | [NOTES](04b_hazard_pointers/NOTES.md) |
| — | [Results](RESULTS.md) | Experiments explaining the surprising benchmark numbers | [RESULTS](RESULTS.md) |

Each `main.cpp` contains the implementation plus its harness. The harnesses check:
- **Exact totals:** the counter must end at exactly N × 1,000,000.
- **Every value delivered exactly once:** no lost, duplicated or garbage values.
- **FIFO order** for the queues, and **LIFO order** in the stack's single-threaded check.
- **Leaks:** nodes created must equal nodes destroyed (hazard pointers).
- **Hangs:** a watchdog ends the run with a TIMEOUT message on deadlock or a lost element.

## Build and run

Requirements: CMake 3.16 or later and a C++20 compiler (Clang or GCC). The harnesses use GCC/Clang inline assembly as a compiler barrier, so MSVC isn't supported.

```bash
# Release build (default)
cmake -S . -B build && cmake --build build
./build/counter
./build/concurrent_queue
./build/spsc_queue
./build/lockfree_stack
./build/hazard_stack

# ThreadSanitizer: data races and memory-order mistakes
cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan
./build-tsan/<target>

# AddressSanitizer: use-after-free, double free
cmake -S . -B build-asan -DSANITIZE=address && cmake --build build-asan
./build-asan/<target>
```

Use only the Release build for timing. The sanitizer builds are 10–50× slower.

The experiments behind [RESULTS.md](RESULTS.md) are standalone files and are not in the CMake build:

```bash
clang++ -std=c++20 -O3 -DNDEBUG experiments/<file>.cpp -o /tmp/exp && /tmp/exp
```

Some experiments are Apple-silicon-specific:
- `stack_backoff.cpp` and `spsc_occupancy.cpp` use the ARM `yield` instruction. On x86 use `pause`.
- `counter_ecores_only.cpp` uses macOS quality-of-service classes to keep threads on the efficiency cores.

## Status

All implementations pass their harness at every tested thread count, and the ThreadSanitizer and AddressSanitizer builds report no errors. The figures below were measured on an Apple M3 Pro (5 performance and 6 efficiency cores, 128-byte cache lines). Take them as indicative: they come from one laptop, and threads can't be pinned to cores on macOS.

| Problem | Result |
|---|---|
| Counter | The atomic is about 3× faster than the mutex up to 5 threads. Past that the mutex is steadier. A per-thread local count is about 400× faster at 16 threads. Padding per-thread slots gives about 100× over packed slots at 8 threads. |
| Concurrent queue | Mutex queue is correct at up to 4 producers and 4 consumers, about 2×10⁷ items/s, flat with thread count. |
| SPSC queue | Lock-free ring is up to about 4.9×10⁸ items/s at small capacities, and about 50× faster than a mutex queue at capacity 2. |
| Lock-free stack | Correct, but slower than a mutex stack under contention: 1.5×10⁶ vs 2.3×10⁷ items/s at 8P/8C. |
| Hazard pointers | Every node is freed, created equals destroyed, at no measurable cost compared with the leaking stack. |

## What the measurements showed

[RESULTS.md](RESULTS.md) has the full details. Several of these findings contradicted the explanation I first gave for the numbers.

- **One shared variable written by many threads doesn't scale, atomic or not.** The cost is moving its cache line between cores. On this machine a contended atomic slows down about 3× as soon as efficiency cores share it.
- **An unfair mutex can beat an atomic under heavy contention.** The lock holder usually re-acquires the lock straight away, so ownership changes only about once per 60–100 operations. The data stays on one core in between.
- **Lock-free means progress, not speed.** The Treiber stack averaged about 6–7 failed `compare_exchange` attempts per operation at 8P/8C. Backoff makes it 6× faster at 2P/2C, but it still loses to a mutex at higher thread counts.
- **Padding fixes false sharing, not true sharing.** Padded per-thread slots in the counter scaled about 100× better than packed ones. Padding the stack's single `head` did nothing.
- **A smaller SPSC ring is faster.** Throughput tracks the ring's size in bytes and falls about 30× from 1 KB to 2 MB. At large sizes the producer is the bottleneck.
- **Microbenchmarks measure themselves easily.** Two examples from this repo:
  - The compiler folded an uncontended counting loop into a constant until a compiler barrier was added.
  - A random-number generator created on every operation made the stack look about 15× slower than it was.

## Concepts covered

- RAII locks: `lock_guard`, `scoped_lock`, `unique_lock`. `mutable` mutexes in `const` methods.
- `std::atomic`: `fetch_add`, `load`/`store`, `compare_exchange_weak` vs `strong`.
- Memory ordering: `relaxed`; `release`/`acquire` pairs to publish data; why a thread reading its own variable needs no ordering; why the failure order of a compare-exchange must be a load order; the store-then-load case that needs `seq_cst`.
- Cache lines, true vs false sharing, `alignas` padding.
- Check-then-act races (TOCTOU) and single-writer designs.
- The ABA problem and safe memory reclamation with hazard pointers.
- Stress-test design: unique self-describing values, edge-case sizes, watchdogs, sanitizers.

## Not done yet

- Hardware-counter profiling (Instruments on macOS, `perf` on Linux) to measure cache misses directly. This would answer the remaining open questions in [RESULTS.md](RESULTS.md).
- A Google Benchmark harness to replace the hand-rolled timing.
- A multi-producer, multi-consumer lock-free queue (Michael-Scott or Vyukov's bounded queue).
- A blocking queue using `condition_variable`.
- Epoch-based reclamation, to compare with hazard pointers.
