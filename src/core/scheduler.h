

#include <vector>
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/wait_object.h"

namespace Core {

struct Ticks {};

struct Cycles {
    Cycles(s64 _count) : count(_count) {}
    operator s64() {
        return count;
    }
    Cycles(std::chrono::nanoseconds ns) : count(ns.count() * BASE_CLOCK_RATE_ARM11 / 1000000000) {}

private:
    s64 count;
};

class SchedulerCore;
class Thread;

class Scheduler {

public:
    Scheduler& GetCore(int core_id) const;
    void ScheduleEvent(Core::TimingEventType* event, Cycles cycles_into_future);
    void RunSlice();
    u64 Ticks() const;

private:
    struct Event {
        Core::TimingEventType* event_type;
        s64 cycles;

        bool operator>(const Event& right) const {
            return cycles > right.cycles;
        }
        bool operator<(const Event& right) const {
            return cycles < right.cycles;
        }
    };

    s64 TimeToNextEvent() const;

    u8 current_core_id;
    std::vector<SchedulerCore> cores;
    std::vector<Event> event_heap;
    s64 current_slice_length;
    s64 max_core_time;

    friend class SchedulerCore;
};

class Thread : public Kernel::WaitObject {

public:
    enum Status { Created, Ready, Running, Yielding, Waiting, Destroyed };

    explicit Thread(SchedulerCore& core);
    virtual ~Thread();

    bool operator>(Thread& right) const {
        return (status * 10000 - priority) > (right.status * 10000 - right.priority);
    }

    void WaitObjectReady(Kernel::WaitObject* object);
    void SetPriority(u32 value) {
        priority = value;
    }

private:
    u32 sequential_id;
    SchedulerCore& core;
    Status status;
    std::unique_ptr<ARM_Interface::ThreadContext> context;
    std::vector<Kernel::WaitObject*> waiting_on;
    u32 priority;

    friend class SchedulerCore;
};

class SchedulerCore {

public:
    explicit SchedulerCore(int core_id);

    void WaitOne(Kernel::WaitObject* object);
    void WaitAny(std::vector<Kernel::WaitObject*> objects);
    void WaitAll(std::vector<Kernel::WaitObject*> objects);
    void Sleep();
    void Sleep(std::chrono::nanoseconds nanoseconds);
    void Stop();

    bool Reschedule();
    Cycles RunSegment(Cycles instruction_count);

    Kernel::Process& Process() const;
    Kernel::Thread& Thread() const;
    ARM_Interface& CPU() const;
    u64 Ticks() const;

    void AddTicks(s64 count) {
        cycles_remaining -= count;
    }

private:
    int core_id;
    Scheduler& root;
    Core::Thread* thread;
    std::vector<Core::Thread*> thread_heap;
    std::vector<Core::Thread*> thread_list;
    s64 cycles_remaining;
    s64 delay_cycles;

    bool CanCutSlice() const {
        return core_id == 0;
    }
    void EndBlock();

    u32 GetSequentialIndex(Core::Thread* thread) {
        auto it = std::find(thread_list.begin(), thread_list.end(), nullptr);
        if (it == thread_list.end()) {
            thread_list.push_back(thread);
        } else {
            *it = thread;
        }
        return thread_list.begin() - it;
    }

    void RemoveThread(Core::Thread* thread) {
        thread_list[thread->sequential_id] = nullptr;
    }

    friend class Scheduler;
    friend class Thread;
};

} // namespace Core
