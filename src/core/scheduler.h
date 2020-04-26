

#include <vector>
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/wait_object.h"

struct Cycles {
    constexpr explicit Cycles(s64 _count) : count(_count) {}
    constexpr explicit operator s64() {
        return count;
    }
    constexpr Cycles(Ticks ticks, float scale_factor)
        : count(static_cast<s64>(s64(ticks) * scale_factor)) {}

    constexpr bool operator<(const Cycles& right) const {
        return count < right.count;
    }
    constexpr bool operator>(const Cycles& right) const {
        return count > right.count;
    }
    constexpr Cycles operator+(const Cycles& right) const {
        return Cycles(count + right.count);
    }
    constexpr Cycles operator-(const Cycles& right) const {
        return Cycles(count - right.count);
    }

private:
    s64 count;
};

struct Ticks {
    constexpr explicit Ticks(s64 _count) : count(_count) {}
    constexpr explicit operator s64() {
        return count;
    }
    constexpr Ticks(std::chrono::nanoseconds ns)
        : count(ns.count() * BASE_CLOCK_RATE_ARM11 / 1000000000) {}
    constexpr Ticks(Cycles cycles, float scale_factor)
        : count(static_cast<s64>(s64(cycles) / scale_factor)) {}

    constexpr bool operator<(const Ticks& right) const {
        return count < right.count;
    }
    constexpr bool operator>(const Ticks& right) const {
        return count > right.count;
    }
    constexpr Ticks operator+(const Ticks& right) const {
        return Ticks(count + right.count);
    }
    constexpr Ticks operator-(const Ticks& right) const {
        return Ticks(count - right.count);
    }

private:
    s64 count;
};

class Kernel::ThreadManager;
class Kernel::Thread;

namespace Core {

class Event {
public:
    virtual const std::string& Name() = 0;
    virtual void Execute(u64 userdata, Ticks cycles_late) = 0;
};

class Scheduler {

public:
    Kernel::ThreadManager& GetCore(int core_id) const;
    void ScheduleEvent(Event* event, Ticks cycles_into_future, u64 userdata = 0);
    void RunSlice();
    Ticks Time_LB() const;
    Ticks Time_UB() const;
    Ticks Time_Current() const;

private:
    struct EventInstance {
        Event* event_type;
        Ticks time;
        u64 userdata;

        bool operator<(const EventInstance& right) const {
            return time < right.time;
        }
    };

    Ticks TimeToNextEvent() const;

    Kernel::ThreadManager& ThreadManager() {
        return cores[current_core_id];
    }

    const Kernel::ThreadManager& ThreadManager() const {
        return cores[current_core_id];
    }

    u8 current_core_id;
    std::vector<Kernel::ThreadManager> cores;
    std::vector<EventInstance> event_heap;
    Ticks current_slice_length;
    Ticks max_core_time;

    friend class Kernel::ThreadManager;
};

} // namespace Core

namespace Kernel {

class Thread : public WaitObject {

public:
    enum Status { Running, Ready, WaitSleep, WaitSyncAll, WaitSyncAny, Created, Destroyed };

    explicit Thread(ThreadManager& core);
    virtual ~Thread();

    bool operator<(Thread& right) const {
        return Order() < right.Order();
    }

    void WaitObjectReady(Kernel::WaitObject* object);
    void SetPriority(u32 value) {
        priority = value;
    }

private:
    int Order() const {
        return status * 64 + priority;
    }

    class WakeupEvent;

    u32 sequential_id;
    ThreadManager& core;
    Status status = Status::Created;
    std::unique_ptr<ARM_Interface::ThreadContext> context;
    std::vector<Kernel::WaitObject*> waiting_on;
    bool wait_all;
    u32 priority = ThreadPriority::Default;
    std::shared_ptr<Kernel::Process> process;
    std::unique_ptr<WakeupEvent> wakeup_event;

    // TODO: Nominal priority + priority boost
    // TODO: Context initialization
    // TODO: TLS

    void WakeUp() {}

    friend class ThreadManager;
};

class ThreadManager {

public:
    explicit ThreadManager(int core_id);

    void WaitOne(Kernel::WaitObject* object);
    void WaitAny(std::vector<Kernel::WaitObject*> objects);
    void WaitAll(std::vector<Kernel::WaitObject*> objects);
    void Sleep();
    void Sleep(std::chrono::nanoseconds nanoseconds);
    void Stop();

    bool Reschedule();
    Ticks RunSegment(Ticks segment_length);

    Kernel::Process& Process() {
        return *thread->process;
    }
    const Kernel::Process& Process() const {
        return *thread->process;
    }
    Kernel::Thread& Thread() {
        return *thread;
    }
    const Kernel::Thread& Thread() const {
        return *thread;
    }
    ARM_Interface& CPU() {
        return *cpu;
    }
    const ARM_Interface& CPU() const {
        return *cpu;
    }

    void AddCycles(Cycles count) {
        cycles_remaining = cycles_remaining - count;
    }

private:
    int core_id;
    Core::Scheduler& root;
    Kernel::Thread* thread;
    std::unique_ptr<ARM_Interface> cpu;
    std::vector<Kernel::Thread*> thread_heap;
    Cycles cycles_remaining;
    Cycles delay_cycles;
    ::Ticks segment_end;

    bool CanCutSlice() const {
        return core_id == 0;
    }
    void EndBlock();

    bool AllThreadsIdle() const;

    void RemoveThread(Kernel::Thread* thread);

    friend class Core::Scheduler;
    friend class Thread;
};

} // namespace Kernel
