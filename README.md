# alfs-cfs-scheduler
Linux CFS-inspired scheduler in C++ using a manual Min-Heap and Unix Domain Sockets.


# 🚀 ALFS – A Linux CFS-Inspired Scheduler in C++

ALFS (Advanced Lightweight Fair Scheduler) is a userspace simulation of the Linux **Completely Fair Scheduler (CFS)**, implemented in modern C++.

Unlike the Linux kernel implementation which uses a Red-Black Tree, ALFS replaces it with a manually implemented **Min-Heap** for task ordering based on `vruntime`.

The scheduler is fully event-driven and communicates via **Unix Domain Socket (UDS)** — with **zero knowledge of future events**, mimicking real OS scheduling behavior.

---

# ✨ Features

* ✅ CFS-inspired fairness model
* 🔥 Manual Min-Heap implementation (no STL `priority_queue`)
* ⚖️ Linux-compatible `nice → weight` mapping
* 🖥 Multi-CPU scheduling
* 🎯 CPU affinity support
* 📦 Cgroup support (shares, quota, mask)
* ⏸ Task blocking / unblocking
* 🔁 Task yield handling
* 📡 Tick-by-tick streaming output
* 📊 Optional scheduling metadata (preemptions, migrations)

---

# 🧠 Scheduling Model

ALFS follows the core CFS principle:

> The task with the smallest `vruntime` runs first.

Each task accumulates virtual runtime as:

```cpp
vruntime += delta_exec * (NICE_0_LOAD / weight);
```

Where:

* `weight` comes from the Linux `nice_to_weight` table
* Lower nice → higher weight → slower `vruntime` growth → more CPU time

---

## 🔄 Task States

* Runnable
* Running
* Blocked
* Exited

On `TASK_UNBLOCK`:

```cpp
task.vruntime = max_vruntime;
```

This prevents unfair advantage after waking up — similar to Linux CFS behavior.

If affinity excludes all CPUs, the task is treated as temporarily unrunnable.

---

# 🏗 Architecture

```
UDS → JSON Event → Event Processor → Scheduler Core → Tick Output
```

### Core Components

### 📡 UDS Listener

* Receives exactly one `TimeFrame` per tick
* `vtime` strictly increases
* No batching, no lookahead

### ⚙️ Event Processor

Handles:

```
TASK_CREATE
TASK_EXIT
TASK_BLOCK
TASK_UNBLOCK
TASK_YIELD
TASK_SETNICE
TASK_SET_AFFINITY
CGROUP_CREATE
CGROUP_MODIFY
CGROUP_DELETE
TASK_MOVE_CGROUP
CPU_BURST
```

### 🗂 Scheduler Core

* Per-CPU run queues
* Global task registry
* Manual Min-Heap ordered by `vruntime`

### 🕒 Tick Generator

Emits exactly one `SchedulerTick` per received `TimeFrame`.

---

# 📥 Input Format

Events are received via UDS in JSON format:

```json
{
  "vtime": 5,
  "events": [
    {
      "action": "TASK_CREATE",
      "taskId": "T1",
      "nice": 0,
      "cgroupId": "root"
    }
  ]
}
```

Guarantees:

* One `TimeFrame` per message
* `vtime` never repeats
* Scheduler has no future knowledge

---

# 📤 Output Format

For every received `TimeFrame`, ALFS immediately emits:

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

Where:

* `schedule[i]` → Task running on CPU `i`
* `"idle"` → No runnable task
* `meta` (optional):

  * `preemptions`
  * `migrations`

---

# 📦 Cgroups

Supported fields:

* `cpuShares`
* `cpuQuotaUs`
* `cpuPeriodUs`
* `cpuMask`

Implements minimum viable hierarchical fairness required for the project.

---

# ⚔️ Differences From Linux CFS

| Linux Kernel          | ALFS                     |
| --------------------- | ------------------------ |
| Red-Black Tree        | Manual Min-Heap          |
| Kernel-space          | Userspace simulation     |
| Interrupt-driven      | Event-driven via UDS     |
| Full cgroup hierarchy | Minimal required support |

---

# 🛠 Build

```bash
g++ -std=c++17 -O2 scheduler.cpp -o alfs
```

---

# ▶ Run

```bash
./alfs <socket_path> <quanta> <cpu_count>
```

If no socket path is provided, it attempts:

```bash
./event.socket
```

---

# 📊 Time Complexity

* Insert task → `O(log n)`
* Extract next task → `O(log n)`
* Per tick scheduling → `O(C log n)`

Where:

* `n` = number of runnable tasks
* `C` = CPU count

---

# 🎯 Design Decisions

* Heap chosen over RB-tree for simplicity and practical speed
* Strict streaming model (no future prediction)
* Simplified `vruntime` fairness model
* Optional metadata to reduce overhead

---

# 📚 References

* Linux CFS implementation:
  [https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c](https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c)
* Linux `nice_to_weight` mapping


# 👨‍💻 Author

Developed as a systems-level scheduler simulation inspired by Linux CFS to deeply understand fairness scheduling and kernel-level design principles.
