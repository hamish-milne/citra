

#include <vector>
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/wait_object.h"

namespace Core {

struct Ticks {};

struct Cycles {};

class Scheduler {

public:
    Scheduler& GetCore(int core_id) const;
    void ScheduleEvent(Core::TimingEventType* event, s64 cycles_into_future);
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

class SchedulerCore {

public:
    explicit SchedulerCore(int core_id);
    void SetThreadPriority(int priority);

    void WaitOne(Kernel::WaitObject* object);
    void WaitAny(std::vector<Kernel::WaitObject*> objects);
    void WaitAll(std::vector<Kernel::WaitObject*> objects);
    void Sleep(s64 nanoseconds);
    void Stop();

    bool Reschedule();
    u64 RunSegment(u64 instruction_count);

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
    std::shared_ptr<Kernel::Process> process;
    Kernel::Thread* thread;
    std::vector<Thread2*> thread_heap;
    s64 cycles_remaining;
    s64 delay_cycles;

    bool CanCutSlice() const {
        return core_id == 0;
    }
    void EndBlock();

    friend class Scheduler;
};

class Thread2 : public Kernel::WaitObject {

public:
    explicit Thread2(SchedulerCore& core);
    virtual ~Thread2();

    bool operator>(Thread2& right) const;

private:
    SchedulerCore& core;
    Kernel::ThreadStatus status;
    std::unique_ptr<ARM_Interface::ThreadContext> context;
    std::vector<Kernel::WaitObject*> waiting_on;

    friend class SchedulerCore;
};

} // namespace Core
