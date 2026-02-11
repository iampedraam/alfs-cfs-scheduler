# 🚀 ALFS – CFS-Inspired Scheduler in C++

ALFS is a userspace simulation of the Linux **Completely Fair Scheduler (CFS)** implemented in modern C++.

It replaces the kernel’s Red-Black Tree with a manually implemented **Min-Heap**, and operates in a fully event-driven model via **Unix Domain Sockets (UDS)** — with no knowledge of future events.

---

## ✨ Features

* CFS-style `vruntime` fairness
* Manual Min-Heap (no STL `priority_queue`)
* Linux-compatible `nice → weight` mapping
* Multi-CPU scheduling
* CPU affinity support
* Basic cgroup support (shares, quota, mask)
* Blocking / unblocking / yield handling
* Tick-by-tick streaming output
* Optional metadata (preemptions, migrations)

---

## 🧠 Scheduling Model

The scheduler always selects the task with the smallest `vruntime`.

```cpp
vruntime += delta_exec * (NICE_0_LOAD / weight);
```

Lower nice → higher weight → slower `vruntime` growth → more CPU time.

On `TASK_UNBLOCK`:

```cpp
task.vruntime = max_vruntime;
```

This prevents unfair advantage after wakeup (similar to Linux CFS behavior).

If a task’s affinity excludes all CPUs, it is treated as temporarily unrunnable.

---

## 🔁 Why Min-Heap Instead of Red-Black Tree?

Linux CFS uses a Red-Black Tree to support ordered traversal and flexible entity removal.

ALFS uses a manual Min-Heap because:

* Only the minimum `vruntime` is required per tick
* Ordered iteration is unnecessary in this simulation
* Simpler implementation in userspace
* Lower structural overhead
* Clear `O(log n)` insertion and extraction

This is a deliberate design trade-off — not a strict improvement over the kernel approach.

---

## 📡 Architecture

```
UDS → JSON TimeFrame → Event Processor → Scheduler Core → Tick Output
```

Per received `TimeFrame`, exactly one `SchedulerTick` is emitted.
No lookahead. No batching. No future knowledge.

---

## 📥 Input

```json
{
  "vtime": 5,
  "events": [
    { "action": "TASK_CREATE", "taskId": "T1", "nice": 0 }
  ]
}
```

* One `TimeFrame` per message
* Strictly increasing `vtime`

---

## 📤 Output

```json
{
  "vtime": 5,
  "schedule": ["T1", "idle", "T2", "idle"],
  "meta": {
    "preemptions": 1,
    "migrations": 0
  }
}
```

`schedule[i]` → task on CPU `i`
`"idle"` → no runnable task

---

## ⚙️ Build

```bash
g++ -std=c++17 -O2 -Wall -Wextra scheduler.cpp -o alfs
```

---

## ▶ Run

```bash
./alfs <socket_path> <quanta> <cpu_count>
```

If no socket path is provided, `./event.socket` is used.

---

## ⏱ Complexity

* Insert → `O(log n)`
* Extract min → `O(log n)`
* Per tick → `O(C log n)`

`n` = runnable tasks
`C` = CPU count

---

## 📘 Academic Context

This project was originally developed as part of an Operating Systems course and later extended to explore CFS design trade-offs and alternative data structures.

---

## 📚 Reference

Linux CFS implementation:
[https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c](https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c)
