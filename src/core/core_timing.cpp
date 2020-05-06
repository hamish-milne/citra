// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <vector>
// #include <cinttypes>
// #include <tuple>
// #include "common/assert.h"
// #include "common/logging/log.h"
#include "core/core_timing.h"
#include "core/hle/kernel/thread.h"

namespace Core {

template <class T>
class min_queue {
    std::vector<T> c;
    std::greater<T> comp;

public:
    template <class... _Valty>
    void emplace(_Valty&&... _Val) {
        c.emplace_back(std::forward<_Valty>(_Val)...);
        std::push_heap(c.begin(), c.end(), comp);
    }

    const T& top() const {
        return c.front();
    }

    void pop() {
        std::pop_heap(c.begin(), c.end(), comp);
        c.pop_back();
    }

    bool empty() const {
        return c.empty();
    }

    template <class Pr>
    bool remove(Pr pred) {
        auto it = std::remove_if(c.begin(), c.end(), pred);
        if (it != c.end()) {
            c.erase(it, c.end());
            std::make_heap(c.begin(), c.end(), comp);
            return true;
        }
        return false;
    }
};

class Timing::EventQueue : public min_queue<Timing::EventInstance> {};

Timing::Timing(u32 core_count, std::function<ARM_Interface*(Timing& timing)> constructor)
    : events(new EventQueue) {
    for (u32 i = 0; i < core_count; i++) {
        cores.emplace_back(i, std::unique_ptr<ARM_Interface>(constructor(*this)));
    }
}

Timing::~Timing() = default;

void Timing::RunSlice() {
    // We assume:
    //  * Threads don't, e.g. write memory at the same time
    //  * Events end with b) a thread waking up, or b) some other change
    //  * Events don't schedule other events to execute immediately

    // Run events scheduled for 'now'. Threshold chosen to reduce small slices.
    while (TimeToNextEvent() < Ticks(100)) {
        auto next_event = events->top();
        next_event.event_type->Execute(*this, next_event.userdata,
                                       Time_Current() - next_event.time);
        events->pop();
    }
    // the max slice time is fairly arbitrary, as it represents the largest duration by which cores
    // can be out of sync. The real maximum value without sacrificing accuracy will depend on
    // software.
    current_slice_length = Ticks(BASE_CLOCK_RATE_ARM11 / 234);
    // Only run the slice til the next event:
    if (TimeToNextEvent() < current_slice_length) {
        current_slice_length = TimeToNextEvent();
    }
    // No cores have run yet, so it's OK to schedule any events we want during this segment
    max_core_time = Time_LB();
    for (auto& core : cores) {
        current_core_id = core.core_id;
        auto cycles_run = core.RunSegment(current_slice_length);
        // Uniquely for the first core, we cut the slice if an event was scheduled before the
        // segment's end. This allows other cores to respond to events scheduled on it. For
        // subsequent cores, however, this isn't practical because core 0 (at least) will be ahead
        // in time already.
        // TODO: Implement core re-ordering
        if (core.CanCutSlice()) {
            current_slice_length = std::min(current_slice_length, cycles_run);
            max_core_time = cycles_run + Time_LB();
        }
    }
}

void Timing::AddTicks(s64 ticks) {
    cores[current_core_id].cycles_remaining =
        cores[current_core_id].cycles_remaining - Cycles(ticks);
}

s64 Timing::GetDowncount() const {
    return s64(cores[current_core_id].cycles_remaining);
}

Ticks Timing::TimeToNextEvent() const {
    if (events->empty()) {
        return Ticks(std::numeric_limits<s64>::max());
    }
    return events->top().time - Time_LB();
}

void Timing::ScheduleEvent(Event* event, Ticks cycles_into_future, u64 userdata) {
    ASSERT(cycles_into_future > Ticks(0));

    if ((Time_Current() + cycles_into_future) < Time_UB()) {
        LOG_WARNING(Core, "Event {} was scheduled {} cycles too soon for all cores to respond",
                    event->Name(), s64(Time_UB() - (Time_Current() + cycles_into_future)));
    }

    // Try to continue the current execution:
    auto& core = cores[current_core_id];
    if (Cycles(cycles_into_future, 1.0) < core.cycles_remaining) {
        // If we're idle, try executing the event now to see if it'll wake us up
        if (core.AllThreadsIdle()) {
            core.AddCycles(Cycles(cycles_into_future, 1.0));
            event->Execute(*this, userdata, Ticks(0));
            core.Reschedule();
            return;
        }
        // If a thread is running, end the block ASAP so we can run a more
        // precise slice.
        core.EndBlock();
    }

    // Push the resultant event onto the heap
    auto scheduled_time = Time_Current() + cycles_into_future;
    events->emplace(EventInstance{event, scheduled_time, userdata});
}

void Timing::UnscheduleEvent(Event* event, u64 userdata) {
    events->remove([=](const auto& e) { return e.event_type == event && e.userdata == userdata; });
}

// // Sort by time, unless the times are the same, in which case sort by the order added to the
// queue bool Timing::Event::operator>(const Timing::Event& right) const {
//     return std::tie(time, fifo_order) > std::tie(right.time, right.fifo_order);
// }

// bool Timing::Event::operator<(const Timing::Event& right) const {
//     return std::tie(time, fifo_order) < std::tie(right.time, right.fifo_order);
// }

// Timing::Timing(std::size_t num_cores, u32 cpu_clock_percentage) {
//     timers.resize(num_cores);
//     for (std::size_t i = 0; i < num_cores; ++i) {
//         timers[i] = std::make_shared<Timer>();
//     }
//     UpdateClockSpeed(cpu_clock_percentage);
//     current_timer = timers[0].get();
// }

// void Timing::UpdateClockSpeed(u32 cpu_clock_percentage) {
//     for (auto& timer : timers) {
//         timer->cpu_clock_scale = 100.0 / cpu_clock_percentage;
//     }
// }

// TimingEventType* Timing::RegisterEvent(const std::string& name, TimedCallback callback) {
//     // check for existing type with same name.
//     // we want event type names to remain unique so that we can use them for serialization.
//     auto info = event_types.emplace(name, TimingEventType{});
//     TimingEventType* event_type = &info.first->second;
//     event_type->name = &info.first->first;
//     if (callback != nullptr) {
//         event_type->callback = callback;
//     }
//     return event_type;
// }

// void Timing::ScheduleEvent(s64 cycles_into_future, const TimingEventType* event_type, u64
// userdata,
//                            std::size_t core_id) {
//     ASSERT(event_type != nullptr);
//     Timing::Timer* timer = nullptr;
//     if (core_id == std::numeric_limits<std::size_t>::max()) {
//         timer = current_timer;
//     } else {
//         ASSERT(core_id < timers.size());
//         timer = timers.at(core_id).get();
//     }

//     s64 timeout = timer->GetTicks() + cycles_into_future;
//     if (current_timer == timer) {
//         // If this event needs to be scheduled before the next advance(), force one early
//         if (!timer->is_timer_sane)
//             timer->ForceExceptionCheck(cycles_into_future);

//         timer->event_queue.emplace_back(
//             Event{timeout, timer->event_fifo_id++, userdata, event_type});
//         std::push_heap(timer->event_queue.begin(), timer->event_queue.end(), std::greater<>());
//     } else {
//         timer->ts_queue.Push(Event{static_cast<s64>(timer->GetTicks() + cycles_into_future), 0,
//                                    userdata, event_type});
//     }
// }

// void Timing::UnscheduleEvent(const TimingEventType* event_type, u64 userdata) {
//     for (auto timer : timers) {
//         auto itr = std::remove_if(
//             timer->event_queue.begin(), timer->event_queue.end(),
//             [&](const Event& e) { return e.type == event_type && e.userdata == userdata; });

//         // Removing random items breaks the invariant so we have to re-establish it.
//         if (itr != timer->event_queue.end()) {
//             timer->event_queue.erase(itr, timer->event_queue.end());
//             std::make_heap(timer->event_queue.begin(), timer->event_queue.end(),
//             std::greater<>());
//         }
//     }
//     // TODO:remove events from ts_queue
// }

// void Timing::RemoveEvent(const TimingEventType* event_type) {
//     for (auto timer : timers) {
//         auto itr = std::remove_if(timer->event_queue.begin(), timer->event_queue.end(),
//                                   [&](const Event& e) { return e.type == event_type; });

//         // Removing random items breaks the invariant so we have to re-establish it.
//         if (itr != timer->event_queue.end()) {
//             timer->event_queue.erase(itr, timer->event_queue.end());
//             std::make_heap(timer->event_queue.begin(), timer->event_queue.end(),
//             std::greater<>());
//         }
//     }
//     // TODO:remove events from ts_queue
// }

// void Timing::SetCurrentTimer(std::size_t core_id) {
//     current_timer = timers[core_id].get();
// }

// s64 Timing::GetTicks() const {
//     return current_timer->GetTicks();
// }

// s64 Timing::GetGlobalTicks() const {
//     return global_timer;
// }

// std::chrono::microseconds Timing::GetGlobalTimeUs() const {
//     return std::chrono::microseconds{GetTicks() * 1000000 / BASE_CLOCK_RATE_ARM11};
// }

// std::shared_ptr<Timing::Timer> Timing::GetTimer(std::size_t cpu_id) {
//     return timers[cpu_id];
// }

// Timing::Timer::Timer() = default;

// Timing::Timer::~Timer() {
//     MoveEvents();
// }

// u64 Timing::Timer::GetTicks() const {
//     u64 ticks = static_cast<u64>(executed_ticks);
//     if (!is_timer_sane) {
//         ticks += slice_length - downcount;
//     }
//     return ticks;
// }

// void Timing::Timer::AddTicks(u64 ticks) {
//     downcount -= static_cast<u64>(ticks * cpu_clock_scale);
// }

// u64 Timing::Timer::GetIdleTicks() const {
//     return static_cast<u64>(idled_cycles);
// }

// void Timing::Timer::ForceExceptionCheck(s64 cycles) {
//     cycles = std::max<s64>(0, cycles);
//     if (downcount > cycles) {
//         slice_length -= downcount - cycles;
//         downcount = cycles;
//     }
// }

// void Timing::Timer::MoveEvents() {
//     for (Event ev; ts_queue.Pop(ev);) {
//         ev.fifo_order = event_fifo_id++;
//         event_queue.emplace_back(std::move(ev));
//         std::push_heap(event_queue.begin(), event_queue.end(), std::greater<>());
//     }
// }

// s64 Timing::Timer::GetMaxSliceLength() const {
//     auto next_event = std::find_if(event_queue.begin(), event_queue.end(),
//                                    [&](const Event& e) { return e.time - executed_ticks > 0; });
//     if (next_event != event_queue.end()) {
//         return next_event->time - executed_ticks;
//     }
//     return MAX_SLICE_LENGTH;
// }

// void Timing::Timer::Advance(s64 max_slice_length) {
//     MoveEvents();

//     s64 cycles_executed = slice_length - downcount;
//     idled_cycles = 0;
//     executed_ticks += cycles_executed;
//     slice_length = max_slice_length;

//     is_timer_sane = true;

//     while (!event_queue.empty() && event_queue.front().time <= executed_ticks) {
//         Event evt = std::move(event_queue.front());
//         std::pop_heap(event_queue.begin(), event_queue.end(), std::greater<>());
//         event_queue.pop_back();
//         if (evt.type->callback != nullptr) {
//             evt.type->callback(evt.userdata, executed_ticks - evt.time);
//         } else {
//             LOG_ERROR(Core, "Event '{}' has no callback", *evt.type->name);
//         }
//     }

//     is_timer_sane = false;

//     // Still events left (scheduled in the future)
//     if (!event_queue.empty()) {
//         slice_length = static_cast<int>(
//             std::min<s64>(event_queue.front().time - executed_ticks, max_slice_length));
//     }

//     downcount = slice_length;
// }

// void Timing::Timer::Idle() {
//     idled_cycles += downcount;
//     downcount = 0;
// }

// s64 Timing::Timer::GetDowncount() const {
//     return downcount;
// }

} // namespace Core
