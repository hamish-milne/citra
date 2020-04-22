

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
    u64 Ticks();

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
    void Stop();

    void SetWaitObjects(std::vector<Kernel::WaitObject> objects);
    void Yield(Kernel::ThreadStatus status);
    void YieldWithEvent(Kernel::ThreadStatus status, Core::TimingEventType* event,
                        s64 cycles_into_future);
    bool Reschedule();

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
    std::vector<Kernel::Thread*> thread_heap;
    s64 cycles_remaining;
    s64 delay_cycles;

    void RunSegment(u64 instruction_count);
    bool AllThreadsIdle();
    void EndBlock();

    friend class Scheduler;
};

} // namespace Core
