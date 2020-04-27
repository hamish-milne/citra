

#include <vector>
#include <queue>
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
    Scheduler(u32 core_count, std::function<ARM_Interface*()> constructor);

    Kernel::ThreadManager& GetCore(int core_id) const;
    void ScheduleEvent(Event* event, Ticks cycles_into_future, u64 userdata = 0);
    void RunSlice();
    Ticks Time_LB() const;
    Ticks Time_UB() const {
        return max_core_time;
    }
    Ticks Time_Current() const;

private:
    struct EventInstance {
        Event* event_type;
        Ticks time;
        u64 userdata;

        bool operator>(const EventInstance& right) const {
            return time > right.time;
        }
    };

    Ticks TimeToNextEvent() const;

    Kernel::ThreadManager& ThreadManager() {
        return cores[current_core_id];
    }

    const Kernel::ThreadManager& ThreadManager() const {
        return cores[current_core_id];
    }

    template <typename T>
    using min_queue = std::priority_queue<T, std::vector<T>, std::greater<T>>;

    u8 current_core_id;
    std::vector<Kernel::ThreadManager> cores;
    min_queue<EventInstance> events;
    Ticks current_slice_length{0};
    Ticks max_core_time{0};

    friend class Kernel::ThreadManager;
};

} // namespace Core

namespace Kernel {

class Thread : public WaitObject {

public:
    enum Status : u32 { Running, Ready, WaitSleep, WaitSyncAll, WaitSyncAny, Created, Destroyed };
    enum Priority : u32 { Min = 0, Default = 23, Max = 64 };

    explicit Thread(ThreadManager& core, std::string name, std::shared_ptr<Kernel::Process> process,
                    VAddr tls_address);
    virtual ~Thread();

    bool operator>(Thread& right) const {
        return Order() > right.Order();
    }

    void WaitObjectReady(Kernel::WaitObject* object);
    void SetPriority(u32 value) {
        priority = value;
    }

private:
    int Order() const {
        return status * Priority::Max + priority;
    }

    class WakeupEvent;

    ThreadManager& core;
    const std::string name;
    const std::unique_ptr<ARM_Interface::ThreadContext> context;
    const std::shared_ptr<Kernel::Process> process;
    const std::unique_ptr<WakeupEvent> wakeup_event;
    const VAddr tls_address;

    std::vector<Kernel::WaitObject*> waiting_on{};
    Status status = Status::Created;
    u32 priority = Priority::Default;

    // TODO: Nominal priority + priority boost

    void WakeUp();

    void SetStatus(Status status);
    std::optional<VAddr> AllocateTLS(KernelSystem& kernel);

    friend class ThreadManager;
};

class ThreadManager {

public:
    explicit ThreadManager(u32 core_id, std::unique_ptr<ARM_Interface> cpu);

    ResultCode WaitOne(Kernel::WaitObject* object, std::chrono::nanoseconds timeout);
    ResultCode WaitAny(std::vector<Kernel::WaitObject*> objects, std::chrono::nanoseconds timeout);
    ResultCode WaitAll(std::vector<Kernel::WaitObject*> objects, std::chrono::nanoseconds timeout);
    void Sleep(std::chrono::nanoseconds nanoseconds);
    void Sleep();
    void Stop();

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

    std::shared_ptr<Kernel::Thread> CreateThread(std::string name, VAddr entry_point, u32 priority,
                                                 u32 arg, VAddr stack_top,
                                                 std::shared_ptr<Kernel::Process> owner_process);

    void AddCycles(Cycles count) {
        cycles_remaining = cycles_remaining - count;
    }

private:
    u32 core_id;
    Core::Scheduler& root;
    Kernel::Thread* thread;
    std::unique_ptr<ARM_Interface> cpu;
    std::vector<Kernel::Thread*> thread_heap;
    Cycles cycles_remaining{0};
    Cycles delay_cycles{0};
    ::Ticks segment_end{0};

    bool Reschedule();
    Ticks RunSegment(Ticks segment_length);

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
