# Problem 4b: Safe Memory Reclamation with Hazard Pointers — Notes

Code: [main.cpp](main.cpp). Build and run:

```
cmake -S . -B build && cmake --build build --target hazard_stack && ./build/hazard_stack
cmake -S . -B build-tsan -DSANITIZE=thread  && cmake --build build-tsan --target hazard_stack && ./build-tsan/hazard_stack
cmake -S . -B build-asan -DSANITIZE=address && cmake --build build-asan --target hazard_stack && ./build-asan/hazard_stack
```

Task: the Treiber stack from Problem 4, but popped nodes must now be freed safely: no use-after-free, no ABA, no double free, no leaks. Harness: sanity check, exactly-once check on values, and a node accounting check (nodes created must equal nodes destroyed). 200,000 items per producer, 1P/1C up to 8P/8C.

## The problem

In a lock-free stack nothing stops thread B from deleting a node that thread A has just loaded but not yet read:

```
A: old = head.load()            // A holds node X
B: pops X and deletes it
A: reads old->next              // use-after-free
```

A node can only be freed when no other thread can still hold a pointer to it. (Also ABA: if X is freed, the allocator may reuse its address, so a later compare_exchange on the same address succeeds wrongly.) Problem 4 avoided this by leaking popped nodes.

## Hazard pointers

- Each thread owns a **hazard slot**: an atomic pointer everyone can read. Slots are padded to a cache line each, since different threads write them (false sharing otherwise).
- **protect**: before dereferencing a node, publish it in your slot, then re-check that it is still what you meant to protect.
- **retire**: a thread that unlinks a node doesn't delete it. It puts the node on its own retire list.
- **scan**: when the list is long enough, read everyone's slots once, delete the retired nodes nobody holds, keep the others for the next scan.

## Implementation

```cpp
Node* protect(std::atomic<Node*>& src) {
    auto& slot = g_slots[this_thread().slot].hazard;
    while (true) {
        Node* p = src.load(acquire);
        if (!p) { slot.store(nullptr, release); return nullptr; }
        slot.store(p, seq_cst);                    // announce
        if (src.load(seq_cst) == p) return p;      // still the top, so protected
    }
}
void clear_protection() { slot.store(nullptr, release); }

void retire(Node* n) {
    list.push_back(n);
    if (list.size() >= 2 * kMaxThreads) scan();
}
void scan() {
    // snapshot all slots (seq_cst loads), then delete retired nodes not in the snapshot
}
```

`pop` is the Problem 4 pop with `protect` instead of a plain load, `clear_protection()` after the value is read, and `retire(old)` instead of leaking.

## Why the re-check in `protect`

If a thread announces a node but a scan ran before the announcement became visible, the node could already be deleted. After announcing, the thread loads `head` again. If it is still the same node, the node had not been popped before the announcement, so anyone who retires it later will see the slot when scanning. If `head` changed, the loop starts over.

## Why `seq_cst` on the announcement

The announcement (a store to the slot) must become visible to other threads before the re-check (a load of `head`). A store followed by a load can be reordered by default, and release/acquire doesn't forbid that particular reordering. Only `seq_cst` (or an explicit fence) does. So the slot store and the re-load of `head` are `seq_cst`, and the loads in `scan()` are `seq_cst` to match. `clear_protection` only needs `release`: it says "I'm done reading", so my earlier reads must finish before the slot clears.

## Why scanning is safe

A node on a retire list is already unlinked. Nobody can newly protect it, because `protect()` re-checks that `head` still points to it. So if it is absent from the snapshot of the slots, nobody holds it and nobody can start. Nodes that were announced stay on the list until a later scan.

## Why the threshold is `2 * kMaxThreads`

At most `kMaxThreads` nodes can be protected at once. A list of at least twice that always has at least `kMaxThreads` deletable entries, so each scan frees at least half the list and scanning costs O(1) amortized per retire.

## Bookkeeping in the scaffold

- Each thread claims a slot with a compare_exchange on `in_use` and releases it at thread exit.
- When a thread exits, any retired nodes it still holds go to a global orphan list (mutex-protected, only touched at thread exit), so they can still be freed.
- The stack destructor frees what is left in the stack, the caller's retire list and the orphans, since all threads have finished by then.

## Results (Release, items/s)

| threads | leaking stack (Problem 4) | hazard pointers | nodes |
|---|---|---|---|
| 1P/1C | 2.5e7–4.5e7 | 2.5e7 | created = destroyed |
| 2P/2C | 6.5e6 | 5.7e6 | created = destroyed |
| 4P/4C | 2.4e6 | 2.6e6 | created = destroyed |
| 8P/8C | 1.5e6 | 1.6e6 | created = destroyed |

ASan: no errors. TSan: no warnings. (Item counts differ between problems: 500,000 per producer in Problem 4, 200,000 here, so compare only the trend.) Safe reclamation cost about nothing extra here, because the stack is bottlenecked on the shared `head` and on `new`/`delete`, not on the hazard machinery.

## Limits of this implementation

- At most `kMaxThreads` (32) threads at a time. A thread-slot registry that grows would remove that.
- One hazard pointer per thread, which is enough for a stack. A queue or list traversal needs two or more.
- `scan` uses a linear search over the snapshot. Sorting it, or using a hash set, matters with many threads.
- The orphan list uses a mutex. That is fine because it is only touched when a thread exits, but it is not lock-free.
- Not shown: epoch-based reclamation (cheaper per access, but a stalled thread can block all reclamation), reference counting, and C++26 `std::hazard_pointer`.

## Takeaways

1. Lock-free structures make you responsible for object lifetime. Passing a stress test with a naive `delete` is luck. Use ASan.
2. "Publish, then re-check" is the core pattern of hazard pointers: the announcement is only valid if the node was still reachable after it.
3. The store-then-load reordering is the case where acquire/release isn't enough. This is where `seq_cst` or a fence is needed.
4. Per-thread slots that other threads read must be padded to avoid false sharing.
5. Track allocations (created vs destroyed) in tests to catch leaks without a leak checker.
