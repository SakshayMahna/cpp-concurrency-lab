# Results: why is it faster or slower?

The problem notes left three performance questions open. This file answers each with experiments. Each section gives the measurements, what they show, and what is still unexplained. Every experiment is a standalone file in [experiments/](experiments/):

```
clang++ -std=c++20 -O3 -DNDEBUG experiments/<file>.cpp -o /tmp/exp && /tmp/exp
```

**Machine:** Apple M3 Pro laptop. 5 performance cores and 6 efficiency cores. 128-byte cache lines. Performance-core L1d 128 KB and L2 16 MB, efficiency-core L1d 64 KB and L2 4 MB.

**Reliability:** most tables are medians of 5–7 runs. Others ran in the background. Large differences (several times) repeated across runs; differences under about 20% did not, so don't read anything into them. macOS doesn't let you pin threads to cores, which adds run-to-run variation.

## Summary

| Question | Answer |
|---|---|
| Why does the atomic counter lose to the mutex at 8–16 threads? | The atomic is fast until efficiency cores join the contention, then gets about 3× slower. The mutex holder keeps the lock for about 60–100 increments in a row, so the counter's cache line moves rarely. |
| Why is the lock-free stack slower than the mutex stack? | Compare-exchange contention on `head` (about 6 failed attempts per operation at 8P/8C). `malloc` is a secondary cost. Backoff helps a lot at 2P/2C but not at 4P/4C or 8P/8C. |
| Why does SPSC throughput fall at large capacities? | It follows the ring's size in bytes. At large sizes the queue is nearly always empty and the producer never waits, so the producer is the bottleneck. Why the producer slows down was not isolated. |

## 1. Atomic counter vs mutex counter

### 1a. Where the atomic falls behind
[counter_thread_sweep.cpp](experiments/counter_thread_sweep.cpp): 1M increments per thread. ns per increment = total wall time / total increments. Median of 7.

| threads | mutex | atomic |
|---|---|---|
| 1 | 7.6 | 2.2 |
| 2 | 8.3 | 3.4 |
| 3 | 15.8 | 5.2 |
| 4 | 17.1 | 6.0 |
| 5 | 17.2 | 7.4 |
| **6** | 19.5 | **20.7** |
| 7 | 19.1 | 26.7 |
| 8 | 17.6 | 38.6 |
| 10 | 16.6 | 24.9 |
| 11 | 17.6 | 28.5 |
| 16 | 18.4 | 28.4 |

The atomic stays fast through 5 threads, then jumps about 3× at the 6th. The machine has 5 performance cores, so the 6th thread has to run on an efficiency core.

`fetch_add(1, relaxed)` compiles to a single `ldadd` instruction on this CPU (checked in the generated assembly). There is no retry loop. So the jump comes from moving the counter's cache line between cores, not from retries.

### 1b. Is it mixing core types, or the efficiency cores themselves?
[counter_ecores_only.cpp](experiments/counter_ecores_only.cpp): the same atomic counter, run once with the default scheduling and once with every thread set to background priority. On Apple silicon, background priority keeps threads on the efficiency cores. That is an OS behavior, not something the test can confirm. Median of 7, ns per increment.

| threads | default | efficiency cores only |
|---|---|---|
| 1 | 4.2 | 2.7 |
| 2 | 4.2 | 5.2 |
| **3** | 5.1 | **21.9** |
| 4 | 6.4 | 20.5 |
| 5 | 7.0 | 20.9 |
| 6 | 16.2 | 22.8 |

With only efficiency cores, the same jump to about 21 ns happens at **3 threads**, with no performance cores involved. So the cause is not mixing core types. The contended atomic costs about 20 ns per operation **once efficiency cores take part**, and the performance cores alone sustain about 7 ns with 5 threads. (The default 6-thread figure here, 16.2, is lower than in 1a, 20.7. That's run-to-run variation.)

### 1c. Why the mutex doesn't degrade the same way
[counter_mutex_handoff.cpp](experiments/counter_mutex_handoff.cpp) counts how often the lock changes owner. Single run per row.

| threads | ns/op | owner changes | increments per owner change |
|---|---|---|---|
| 1 | 12.3 | 1 | 1,000,000 |
| 2 | 14.0 | 2,710 | 738 |
| 4 | 40.9 | 63,350 | 63 |
| 5 | 19.3 | 63,796 | 78 |
| 6 | 19.0 | 73,028 | 82 |
| 8 | 18.2 | 81,902 | 98 |
| 11 | 18.0 | 125,087 | 88 |
| 16 | 19.4 | 199,638 | 80 |

With 4 or more threads, the lock changes owner only once every 60–100 increments. A thread that releases the lock usually re-acquires it straight away, before a waiting thread gets it, so `std::mutex` here is not fair. The counter's cache line therefore stays on one core for long runs. The atomic, by contrast, can move the line on every increment. That explains why the mutex stays flat.

The 40.9 ns at 4 threads didn't appear in 1a (17.1) and is likely noise. I tried to count how often threads sleep with `getrusage`, but it reported 0 voluntary context switches in every case. macOS doesn't seem to fill in that field reliably, so that question stays open.

**Conclusion:** the atomic is cheaper per operation. But under heavy contention it can move the cache line on every increment, and that gets expensive once efficiency cores take part. An unfair mutex groups many increments on one core, so it moves the line much less often. It's slower per operation but much steadier.

## 2. Lock-free stack vs mutex stack

### 2a. Allocation vs contention
[stack_malloc_vs_cas.cpp](experiments/stack_malloc_vs_cas.cpp). Producers push 200,000 items each and consumers pop until everything is consumed. Throughput is in millions of items per second, median of 5.

- **"new in push"** allocates each node inside the timed push.
- **"preallocated"** allocates every node before timing starts, which takes `malloc` out of the picture.
- **"mutex linked list"** uses the same nodes as the lock-free stack, so it's a like-for-like comparison.

| threads | structure | new in push | preallocated |
|---|---|---|---|
| 1P/1C | lock-free | 25.9 | 96.4 |
| | mutex linked list | 18.0 | 67.1 |
| | mutex `std::stack` | 21.8 | 36.7 |
| 2P/2C | lock-free | 5.7 | 8.9 |
| | mutex linked list | 9.1 | 24.1 |
| | mutex `std::stack` | 10.2 | 24.7 |
| 4P/4C | lock-free | 2.0 | 2.1 |
| | mutex linked list | 7.8 | 21.9 |
| | mutex `std::stack` | 8.4 | 25.1 |
| 8P/8C | lock-free | 1.4 | 1.5 |
| | mutex linked list | 7.4 | 22.7 |
| | mutex `std::stack` | 6.9 | 24.6 |

- With one producer and one consumer, lock-free wins (96 vs 67 when preallocated), because contention is low.
- Preallocating helps every version, up to 3× for the mutex ones. So `malloc` matters, but it isn't the gap: at 8P/8C, preallocated lock-free is 1.5 against 22.7 for the mutex linked list.
- At 8P/8C there were **7.2 failed compare-exchange attempts per operation**. Each failure is a wasted attempt plus a cache-line transfer. The mutex makes threads wait their turn, so the line moves far less often (see 1c).

### 2b. Does backoff help?
[stack_backoff.cpp](experiments/stack_backoff.cpp): nodes preallocated. After each failed compare-exchange, the thread spins a random number of times. The limit doubles with each failure, up to a maximum of 64 or 1024. Median of 5.

| threads | no backoff | backoff ≤64 | backoff ≤1024 | mutex linked list |
|---|---|---|---|---|
| 1P/1C | 72.4 (0.04) | 75.1 (0.05) | 128.0 (0.01) | 63.2 |
| 2P/2C | 15.1 (0.70) | 16.6 (0.74) | **93.6** (0.05) | 27.4 |
| 4P/4C | 2.1 (4.22) | 2.0 (4.38) | 2.4 (3.21) | 23.6 |
| 8P/8C | 1.5 (6.10) | 1.5 (6.24) | 2.2 (3.93) | 22.6 |

Values are millions of items per second. The number in brackets is failed compare-exchange attempts per operation.

- Large backoff makes lock-free about 6× faster at 2P/2C, and **3.4× faster than the mutex** there.
- At 4P/4C and 8P/8C it cuts failed attempts by about a third but still stays about 10× slower than the mutex.
- Small backoff (≤64 spins) is too short to make any difference.
- **Measurement note:** the first version of this experiment created a new random-number generator from `std::random_device` on every push and pop. That is a system call, and it made every lock-free row look about 15× slower. It's fixed now, with one generator per thread. It's a good example of benchmark code measuring itself instead of the thing under test.

**Conclusion:** a lock-free stack guarantees progress (a stalled thread can't block the others). It does not guarantee speed. With one shared `head` and heavy contention, a lock that groups operations wins clearly. Backoff closes the gap only at moderate contention. Going further would need a different design, such as an elimination array or per-thread stacks.

## 3. SPSC queue throughput vs capacity

### 3a. Capacity sweep
[spsc_capacity_sweep.cpp](experiments/spsc_capacity_sweep.cpp): the final SPSC queue (padded indices, cached other index), 10M items, `int` slots, median of 5.

| capacity | ring size | M items/s |
|---|---|---|
| 64 | 0.25 KB | 437 |
| 256 | 1 KB | 485 |
| 1024 | 4 KB | 366 |
| 2048 | 8 KB | 295 |
| 4096 | 16 KB | 134 |
| 8192 | 32 KB | 59 |
| 16384 | 64 KB | 31 |
| 32768 | 128 KB | 23 |
| 65536 | 256 KB | 17 |
| 131072 | 512 KB | 15 |
| 524288 | 2 MB | 14 |

Throughput falls about 30× from the smallest ring to the largest. The decline is gradual and starts around 8–16 KB, with no sharp drop at the 128 KB L1 size.

### 3b. Slot count or bytes?
[spsc_bytes_vs_slots.cpp](experiments/spsc_bytes_vs_slots.cpp): rings of the same size in bytes, built from 1-byte slots and from 4-byte slots. Median of 5.

| ring size | char slots | int slots |
|---|---|---|
| 16 KB | 110 | 187 |
| 64 KB | 30 | 34 |
| 256 KB | 17.7 | 17.0 |
| 1 MB | 14.7 | 14.3 |

From 64 KB up, throughput depends on the ring's size in **bytes**, not the number of slots. So it's a memory-footprint effect, not the index or wrap-around logic. At 16 KB the slot count still matters.

### 3c. Which side is waiting?
[spsc_occupancy.cpp](experiments/spsc_occupancy.cpp) records how full the ring is each time the consumer pops, and how often each side finds the ring full or empty. First run, no consumer pause:

| capacity | M items/s | average items in ring | producer found it full | consumer found it empty |
|---|---|---|---|---|
| 256 | 294 | 42 | 637,694 | 674,628 |
| 1024 | 248 | 87 | 1,342,279 | 5,220,940 |
| 4096 | 116 | 83 | 130,886 | 30,543,649 |
| 16384 | 45 | 31 | 0 | 119,055,539 |
| 65536 | 18.5 | 7 | 0 | 335,512,307 |
| 262144 | 17.6 | 530 | 0 | 288,562,507 |

- From 16K slots up, the producer **never** found the ring full, and the consumer found it empty hundreds of millions of times.
- The ring held only a handful of items out of tens of thousands of slots. The repeat runs agreed on this.
- So at large sizes the producer is the bottleneck and the consumer is waiting on it. My earlier guess, that the producer runs far ahead and the consumer reads data written long ago, is wrong.

### 3d. Is the waiting consumer slowing the producer?
While the ring is empty, every failed pop reloads `tail`. That pulls the producer's `tail` cache line away from it each time. To test whether this matters, the same experiment was run with the consumer spinning 64 times before retrying an empty ring.

The pause cut the consumer's empty checks by roughly 10–40×. Throughput at 64K and 256K slots stayed at about 15–21 million items per second across three runs, with or without the pause. One run showed 41.5 at 256K slots, but the repeats didn't confirm it. Rings of 1K slots and smaller varied by up to 4× between runs either way, so the pause can't be judged there.

So the consumer's polling isn't what limits large rings.

### 3e. Still open
Why does the producer slow down as the ring grows in bytes? One plausible cause is that each new cache line the producer writes was last touched a full lap of the ring ago. In a large ring that line has dropped out of the nearby caches, so each write has to fetch it from farther away.

Confirming this needs hardware cache-miss counters: Instruments' CPU Counters on macOS, or `perf` on Linux. Those haven't been run.

**Conclusion:** for SPSC, a small ring that stays in cache is fastest. A larger buffer only helps if the producer and consumer need to drift far apart in time. It does not make the queue faster.

## Corrections to earlier explanations

| Earlier claim | Status after measuring |
|---|---|
| The atomic counter loses at high thread counts because it keeps retrying (Problem 1) | **Wrong.** `fetch_add` is one `ldadd` instruction, with no retries. The slowdown starts once efficiency cores share the counter. |
| The mutex counter copes because blocked threads sleep (Problem 1) | **Not confirmed.** Measured instead: the lock changes owner only once every 60–100 increments. |
| The SPSC queue slows at 65536 slots because the ring is bigger than L1 (Problem 3) | **Not supported.** The decline is gradual from about 8–16 KB, follows the ring's size in bytes, and the producer is the bottleneck. |
| The lock-free stack is slow because of `malloc` (Problem 4) | **Partly true.** `malloc` is a secondary cost; compare-exchange contention dominates. |
| `alignas` on the stack's `head` doesn't help (Problem 4) | **Confirmed.** Every thread writes `head`, which padding can't fix. |

## Remaining open questions

- What makes the SPSC producer slow down at large rings? Needs hardware cache-miss counters (3e).
- How often do threads sleep in the mutex counter? macOS `getrusage` didn't report it (1c).
- Would the counter crossover look the same on a machine whose cores are all one type? That needs different hardware (1b).
