// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <unordered_map>
#include "common/archives.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

SERIALIZE_EXPORT_IMPL(Kernel::Timer)

namespace Kernel {

/// The timer callback event, called when a timer is fired
class Timer::Callback : public Core::Event {
    Timer& parent;

public:
    explicit Callback(Timer& parent_) : parent(parent_) {}

    const std::string& Name() const override {
        static const std::string name = "TimerCallback";
        return name;
    }

    void Execute(Core::Timing& timing, u64 callback_id, Ticks cycles_late) override {
        parent.Signal(cycles_late);
    }
};

Timer::Timer(KernelSystem& kernel)
    : WaitObject(kernel), kernel(kernel), timer_event(new Callback(*this)) {}
Timer::~Timer() {
    Cancel();
    // timer_manager.timer_callback_table.erase(callback_id);
}

std::shared_ptr<Timer> KernelSystem::CreateTimer(ResetType reset_type, std::string name) {
    auto timer{std::make_shared<Timer>(*this)};

    timer->reset_type = reset_type;
    timer->signaled = false;
    timer->name = std::move(name);
    timer->initial_delay = Ticks(0);
    timer->interval_delay = Ticks(0);
    // timer->callback_id = ++timer_manager->next_timer_callback_id;
    // timer_manager->timer_callback_table[timer->callback_id] = timer.get();

    return timer;
}

bool Timer::ShouldWait(const Thread* thread) const {
    return !signaled;
}

void Timer::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    if (reset_type == ResetType::OneShot)
        signaled = false;
}

void Timer::Set(Ticks initial, std::optional<Ticks> interval) {
    // Ensure we get rid of any previous scheduled event
    Cancel();

    initial_delay = initial;
    interval_delay = boost::optional<Ticks>(interval.has_value(), interval.value_or(Ticks(0)));

    if (initial < Ticks(1)) {
        // Immediately invoke the callback
        Signal(Ticks(0));
    } else {
        kernel.timing.ScheduleEvent(timer_event.get(), initial);
    }
}

void Timer::Cancel() {
    kernel.timing.UnscheduleEvent(timer_event.get());
}

void Timer::Clear() {
    signaled = false;
}

void Timer::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();

    if (reset_type == ResetType::Pulse)
        signaled = false;
}

void Timer::Signal(Ticks cycles_late) {
    LOG_TRACE(Kernel, "Timer {} fired", GetObjectId());

    signaled = true;

    // Resume all waiting threads
    WakeupAllWaitingThreads();

    if (interval_delay.has_value()) {
        // Reschedule the timer with the interval delay
        kernel.timing.ScheduleEvent(timer_event.get(), interval_delay.value() - cycles_late);
    }
}

// TimerManager::TimerManager(Core::Timing& timing) : timing(timing) {
//     timer_callback_event_type =
//         timing.RegisterEvent("TimerCallback", [this](u64 thread_id, s64 cycle_late) {
//             TimerCallback(thread_id, cycle_late);
//         });
// }

} // namespace Kernel
