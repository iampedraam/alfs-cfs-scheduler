#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;


static const int NICE_0_LOAD = 1024;
static const int NICE_MIN = -20;
static const int NICE_MAX = 19;

static const int NICE_TO_WEIGHT[40] = {
    88761, 71755, 56483, 46273, 36291,
    29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906,
    3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423,
    335, 272, 215, 172, 137,
    110, 87, 70, 56, 45,
    36, 29, 23, 18, 15
};
static int clamp_nice(int nice) {
    if (nice < NICE_MIN) return NICE_MIN;
    if (nice > NICE_MAX) return NICE_MAX;
    return nice;
}
static int nice_to_weight(int nice) {
    nice = clamp_nice(nice);
    return NICE_TO_WEIGHT[nice - NICE_MIN];
}

static vector<uint8_t> maskAll(int cpuCount) {
    vector<uint8_t> m(cpuCount, 1);
    return m;
}
static vector<uint8_t> maskFromJson(int cpuCount, const json& arr) {
    vector<uint8_t> m(cpuCount, 0);
    for (const auto& v : arr) {
        int cpu = v.get<int>();
        if (cpu >= 0 && cpu < cpuCount) m[cpu] = 1;
    }
    return m;
}

static bool isCpuAllowed(const vector<uint8_t>& m, int cpu) {
    if (cpu < 0 || cpu >= (int)m.size()) return false;
    return m[cpu] != 0;
}
static bool isAnyCpuAvailable(const vector<uint8_t>& m) {
    for (auto x : m) if (x) return true;
    return false;
}

enum class TaskState : uint8_t { RUNNABLE = 0, BLOCKED = 1, EXITED = 2 };

struct Cgroup {
    string id;
    int cpuShares = 1024; // default
    int cpuQuotaUs = -1;
    int cpuPeriodUs = 100000;
    vector<uint8_t> cpuMask;

    Cgroup() = default;
};

struct Task {
    string id;
    int nice = 0;
    int weight = NICE_0_LOAD;
    string cgroupId = "0";
    TaskState state = TaskState::RUNNABLE;

    int64_t vruntime = 0;

    vector<uint8_t> affinityMask;
    int lastCpu = -1;

    int burstRemaining = 0;
};


struct MinHeap {
    vector<string> a;
    unordered_map<string, int> pos;
    unordered_map<string, Task>* tasks = nullptr;

    MinHeap() = default;

    bool contains(const string& id) const {
        return pos.find(id) != pos.end();
    }

    bool empty() const { return a.empty(); }
    int size() const { return (int)a.size(); }

    bool lessId(const string& x, const string& y) const {
        const Task& tx = tasks->at(x);
        const Task& ty = tasks->at(y);
        if (tx.vruntime != ty.vruntime) return tx.vruntime < ty.vruntime;

        return x < y;
    }

    void swapAt(int i, int j) {
        string tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
        pos[a[i]] = i;
        pos[a[j]] = j;
    }

    void siftUp(int i) {
        while (i > 0) {
            int p = (i - 1) / 2;
            if (lessId(a[i], a[p])) {
                swapAt(i, p);
                i = p;
            } else break;
        }
    }
    void siftDown(int i) {
        int n = (int)a.size();
        while (true) {
            int l = 2 * i + 1;
            int r = 2 * i + 2;
            int smallest = i;

            if (l < n && lessId(a[l], a[smallest])) smallest = l;
            if (r < n && lessId(a[r], a[smallest])) smallest = r;

            if (smallest != i) {
                swapAt(i, smallest);
                i = smallest;
            } else break;
        }
    }

    void push(const string& id) {
        if (contains(id)) return;
        a.push_back(id);
        int i = (int)a.size() - 1;
        pos[id] = i;
        siftUp(i);
    }

    string popMin() {
        if (a.empty()) return "";
        std::string out = a[0];
        remove(out);
        return out;
    }
    void remove(const string& id) {
        auto it = pos.find(id);
        if (it == pos.end()) return;
        int i = it->second;
        int n = (int)a.size();
        pos.erase(it);

        if (i == n - 1) {
            a.pop_back();
            return;
        }
        a[i] = a[n - 1];
        a.pop_back();
        pos[a[i]] = i;

        siftUp(i);
        siftDown(i);
    }

    void fixKey(const string& id) {
        auto it = pos.find(id);
        if (it == pos.end()) return;
        int i = it->second;
        siftUp(i);
        siftDown(i);
    }
};


static bool getOneObjectJSON(string& buf, string& out) {

    // Find first '{'
    size_t start = buf.find('{');
    if (start == string::npos) return false;

    int depth = 0;
    bool inStr = false;
    bool esc = false;

    for (size_t i = start; i < buf.size(); i++) {
        char c = buf[i];

        if (inStr) {
            if (esc) {
                esc = false;
            } else {
                if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
            }
            continue;
        } else {
            if (c == '"') {
                inStr = true;
                continue;
            }
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    out = buf.substr(start, i - start + 1);
                    buf.erase(0, i + 1);
                    return true;
                }
            }
        }
    }
    return false;
}

static int connect_uds(const string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}


// Scheduler
struct Scheduler {
    int cpuCount = 1;
    int64_t quanta = 1;

    unordered_map<string, Task> tasks;
    unordered_map<string, Cgroup> cgroups;

    MinHeap rq;

    int64_t maxVruntime = 0;

    vector<string> lastSchedule;

    Scheduler(int cpuCount_, int64_t quanta_)
        : cpuCount(cpuCount_), quanta(quanta_) {
        rq.tasks = &tasks;
        lastSchedule.assign(cpuCount, "idle");

        Cgroup root;
        root.id = "0";
        root.cpuShares = 1024;
        root.cpuQuotaUs = -1;
        root.cpuPeriodUs = 100000;
        root.cpuMask = maskAll(cpuCount);
        cgroups[root.id] = root;
    }

    bool taskExists(const string& id) const {
        return tasks.find(id) != tasks.end();
    }

    Cgroup& getCgroup(const string& id) {
        auto it = cgroups.find(id);
        if (it != cgroups.end()) return it->second;
        return cgroups["0"];
    }

    bool canTaskRunOnCPU(const Task& t, int cpu) {
        if (t.state != TaskState::RUNNABLE) return false;

        if (!isCpuAllowed(t.affinityMask, cpu)) return false;

        // cgroup mask
        auto cgIt = cgroups.find(t.cgroupId);
        const Cgroup& cg = (cgIt != cgroups.end()) ? cgIt->second : cgroups["0"];
        if (!isCpuAllowed(cg.cpuMask, cpu)) return false;

        // if mask = no allowed cpu => unrunnable
        return true;
    }

    int effectiveWeight(const Task& t) {
        int w = t.weight;
        auto cgIt = cgroups.find(t.cgroupId);
        int shares = 1024;
        if (cgIt != cgroups.end()) shares = cgIt->second.cpuShares;

        int64_t ew = (int64_t)w * (int64_t)shares;
        ew /= 1024;
        if (ew < 1) ew = 1;
        if (ew > 2000000000LL) ew = 2000000000LL;
        return (int)ew;
    }

    int64_t vruntimeDelta(const Task& t) {
        int ew = effectiveWeight(t);

        int64_t num = quanta * (int64_t)NICE_0_LOAD;
        int64_t d = num / ew;
        if (d <= 0) d = 1;
        return d;
    }

    void ensureTaskMasks(Task& t) {
        if ((int)t.affinityMask.size() != cpuCount) {
            t.affinityMask = maskAll(cpuCount);
        }
    }


    // Events
    void taskCreate(const json& e) {
        string id = e.at("taskId").get<string>();
        if (taskExists(id)) {
            return;
        }

        Task t;
        t.id = id;
        t.nice = 0;
        if (e.contains("nice") && !e["nice"].is_null()) t.nice = e["nice"].get<int>();
        t.nice = clamp_nice(t.nice);
        t.weight = nice_to_weight(t.nice);

        t.cgroupId = "0";
        if (e.contains("cgroupId") && !e["cgroupId"].is_null()) t.cgroupId = e["cgroupId"].get<std::string>();
        if (cgroups.find(t.cgroupId) == cgroups.end()) t.cgroupId = "0";

        t.state = TaskState::RUNNABLE;
        t.vruntime = maxVruntime;
        t.affinityMask = maskAll(cpuCount);
        t.lastCpu = -1;
        t.burstRemaining = 0;

        tasks[id] = t;
        rq.push(id);
    }

    void taskExit(const json& e) {
        string id = e.at("taskId").get<string>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;

        if (rq.contains(id)) rq.remove(id);
        t.state = TaskState::EXITED;
    }

    void taskBlock(const json& e) {
        string id = e.at("taskId").get<string>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;

        if (t.state == TaskState::EXITED) return;

        // Burst prevents blocking
        if (t.burstRemaining > 0) return;

        if (rq.contains(id)) rq.remove(id);
        t.state = TaskState::BLOCKED;
    }

    void taskUnblock(const json& e) {
        string id = e.at("taskId").get<string>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;

        if (t.state == TaskState::EXITED) return;

        t.vruntime = maxVruntime;
        t.state = TaskState::RUNNABLE;
        ensureTaskMasks(t);
        rq.push(id);
    }

    void taskYield(const json& e) {
        string id = e.at("taskId").get<string>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;

        if (t.state != TaskState::RUNNABLE) return;

        t.vruntime = maxVruntime;
        if (rq.contains(id)) rq.fixKey(id);
    }

    void taskSetnice(const json& e) {
        string id = e.at("taskId").get<string>();
        int nn = e.at("newNice").get<int>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;
        if (t.state == TaskState::EXITED) return;

        t.nice = clamp_nice(nn);
        t.weight = nice_to_weight(t.nice);

        if (rq.contains(id)) rq.fixKey(id);
    }

    void taskSetAffinity(const json& e) {
        string id = e.at("taskId").get<string>();
        auto it = tasks.find(id);
        if (it == tasks.end()) return;
        Task& t = it->second;
        if (t.state == TaskState::EXITED) return;

        const json& arr = e.at("cpuMask");
        t.affinityMask = maskFromJson(cpuCount, arr);
    }

    void cgroupCreate(const json& e) {
        Cgroup cg;
        cg.id = e.at("cgroupId").get<string>();
        cg.cpuShares = 1024;
        if (e.contains("cpuShares") && !e["cpuShares"].is_null()) cg.cpuShares = e["cpuShares"].get<int>();
        if (cg.cpuShares < 1) cg.cpuShares = 1;

        cg.cpuQuotaUs = -1;
        if (e.contains("cpuQuotaUs") && !e["cpuQuotaUs"].is_null()) cg.cpuQuotaUs = e["cpuQuotaUs"].get<int>();

        cg.cpuPeriodUs = 100000;
        if (e.contains("cpuPeriodUs") && !e["cpuPeriodUs"].is_null()) cg.cpuPeriodUs = e["cpuPeriodUs"].get<int>();

        if (e.contains("cpuMask") && !e["cpuMask"].is_null()) cg.cpuMask = maskFromJson(cpuCount, e["cpuMask"]);
        else cg.cpuMask = maskAll(cpuCount);

        cgroups[cg.id] = cg;
    }

    void cgroupModify(const json& e) {
        string id = e.at("cgroupId").get<string>();
        auto it = cgroups.find(id);
        if (it == cgroups.end()) {
            
            Cgroup cg;
            cg.id = id;
            cg.cpuShares = 1024;
            cg.cpuQuotaUs = -1;
            cg.cpuPeriodUs = 100000;
            cg.cpuMask = maskAll(cpuCount);
            cgroups[id] = cg;
            it = cgroups.find(id);
        }

        Cgroup& cg = it->second;

        if (e.contains("cpuShares") && !e["cpuShares"].is_null()) {
            cg.cpuShares = e["cpuShares"].get<int>();
            if (cg.cpuShares < 1) cg.cpuShares = 1;
        }
        if (e.contains("cpuQuotaUs") && !e["cpuQuotaUs"].is_null()) {
            cg.cpuQuotaUs = e["cpuQuotaUs"].get<int>();
        }
        if (e.contains("cpuPeriodUs") && !e["cpuPeriodUs"].is_null()) {
            cg.cpuPeriodUs = e["cpuPeriodUs"].get<int>();
        }
        if (e.contains("cpuMask") && !e["cpuMask"].is_null()) {
            cg.cpuMask = maskFromJson(cpuCount, e["cpuMask"]);
        }

    }

    void cgroupDelete(const json& e) {
        string id = e.at("cgroupId").get<string>();
        if (id == "0") return;

        for (auto& kv : tasks) {
            Task& t = kv.second;
            if (t.cgroupId == id && t.state != TaskState::EXITED) {
                t.cgroupId = "0";
            }
        }
        cgroups.erase(id);
    }

    void taskMoveCgroup(const json& e) {
        string tid = e.at("taskId").get<string>();
        string cg = e.at("newCgroupId").get<string>();
        auto it = tasks.find(tid);
        if (it == tasks.end()) return;
        Task& t = it->second;
        if (t.state == TaskState::EXITED) return;

        if (cgroups.find(cg) == cgroups.end()) cg = "0";
        t.cgroupId = cg;
    }

    void CpuBurst(const json& e) {
        string tid = e.at("taskId").get<string>();
        int dur = e.at("duration").get<int>();
        if (dur < 0) dur = 0;
        auto it = tasks.find(tid);
        if (it == tasks.end()) return;
        Task& t = it->second;
        if (t.state == TaskState::EXITED) return;
        t.burstRemaining = dur;
    }

    void applyEvent(const json& e) {
        string action = e.at("action").get<string>();

        if (action == "TASK_CREATE") taskCreate(e);
        else if (action == "TASK_EXIT") taskExit(e);
        else if (action == "TASK_BLOCK") taskBlock(e);
        else if (action == "TASK_UNBLOCK") taskUnblock(e);
        else if (action == "TASK_YIELD") taskYield(e);
        else if (action == "TASK_SETNICE") taskSetnice(e);
        else if (action == "TASK_SET_AFFINITY") taskSetAffinity(e);

        else if (action == "CGROUP_CREATE") cgroupCreate(e);
        else if (action == "CGROUP_MODIFY") cgroupModify(e);
        else if (action == "CGROUP_DELETE") cgroupDelete(e);
        else if (action == "TASK_MOVE_CGROUP") taskMoveCgroup(e);

        else if (action == "CPU_BURST") CpuBurst(e);
        else {
            // Unknown action ignore
        }
    }

    json runTick(int64_t vtime, const json& events) {
        for (const auto& e : events) applyEvent(e);

        vector<string> schedule(cpuCount, "idle");
        vector<string> chosen(cpuCount, "");
        vector<string> stashed;

        // pick for each CPU
        for (int cpu = 0; cpu < cpuCount; cpu++) {
            string pick = "";

            while (!rq.empty()) {
                std::string cand = rq.popMin();
                auto it = tasks.find(cand);
                if (it == tasks.end()) continue;
                Task& t = it->second;

                if (t.state != TaskState::RUNNABLE) {
                    continue;
                }

                if (!isAnyCpuAvailable(t.affinityMask)) {
                    stashed.push_back(cand);
                    continue;
                }

                auto cgIt = cgroups.find(t.cgroupId);
                const Cgroup& cg = (cgIt != cgroups.end()) ? cgIt->second : cgroups["0"];
                if (!isAnyCpuAvailable(cg.cpuMask)) {
                    stashed.push_back(cand);
                    continue;
                }

                if (canTaskRunOnCPU(t, cpu)) {
                    pick = cand;
                    break;
                } else {
                    stashed.push_back(cand);
                }
            }

            if (!pick.empty()) {
                schedule[cpu] = pick;
                chosen[cpu] = pick;
            } else {
                schedule[cpu] = "idle";
            }

            for (const auto& id : stashed) rq.push(id);
            stashed.clear();
        }

        int preemptions = 0;
        int migrations = 0;

        for (int cpu = 0; cpu < cpuCount; cpu++) {
            string cur = schedule[cpu];
            string prev = lastSchedule[cpu];

            if (cur != prev) {
                if (prev != "idle") preemptions++;
            }

            if (cur != "idle") {
                Task& t = tasks[cur];

                if (t.lastCpu != -1 && t.lastCpu != cpu) migrations++;

                t.lastCpu = cpu;

                // increment vruntime
                int64_t d = vruntimeDelta(t);
                t.vruntime += d;
                if (t.vruntime > maxVruntime) maxVruntime = t.vruntime;

                // burst counting
                if (t.burstRemaining > 0) {
                    t.burstRemaining--;
                }

                if (t.state == TaskState::RUNNABLE) {
                    rq.push(cur);
                    rq.fixKey(cur);
                }
            }
        }

        lastSchedule = schedule;

        // Build output
        json out;
        out["vtime"] = vtime;
        out["schedule"] = schedule;

        json meta;
        meta["preemptions"] = preemptions;
        meta["migrations"] = migrations;
        out["meta"] = meta;

        return out;
    }
};






int main(int argc, char** argv) {
    string sockPath = "./event.socket";
    int64_t quanta = 0;
    int cpuCount = 0;

    if (argc == 3) {
        quanta = atoll(argv[1]);
        cpuCount = atoi(argv[2]);
    } else if (argc == 4) {
        sockPath = argv[1];
        quanta = atoll(argv[2]);
        cpuCount = atoi(argv[3]);
    } else {
        cerr << "Usage:\n"
            << "  " << argv[0] << " [socket_path] <quanta> <cpu_count>\n"
            << "If socket_path omitted, uses ./event.socket\n";
        return 2;
    }

    if (quanta <= 0) quanta = 1;
    if (cpuCount <= 0) cpuCount = 1;

    int fd = connect_uds(sockPath);
    if (fd < 0) {
        cerr << "Failed to connect UDS at '" << sockPath << "': " << strerror(errno) << "\n";
        return 1;
    }

    Scheduler sched(cpuCount, quanta);

    string buf;
    buf.reserve(1 << 16);


    //Read inputs
    while (true) {
        char tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            cerr << "recv error: " << strerror(errno) << "\n";
            break;
        }

        buf.append(tmp, tmp + n);

        while (true) {
            string one;
            if (!getOneObjectJSON(buf, one)) break;

            json tf;
            try {
                tf = json::parse(one);
            } catch (...) {
                continue;
            }

            if (!tf.contains("vtime") || !tf.contains("events")) continue;
            int64_t vtime = 0;
            try { vtime = tf["vtime"].get<int64_t>(); } catch (...) { continue; }

            json events = tf["events"];
            if (!events.is_array()) continue;

            json tick = sched.runTick(vtime, events);

            //Output
            cout << tick.dump() << "\n";
            cout.flush();
        }
    }

    close(fd);
    return 0;
}