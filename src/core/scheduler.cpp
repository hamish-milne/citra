#include <algorithm>
#include "core/core_timing.h"
#include "scheduler.h"

namespace Core {

void Scheduler::RunSlice() {
    // Only run til the next event
    auto next_event = event_heap.begin();
    if (next_event != event_heap.end() && next_event->cycles < (Ticks() + current_slice_length)) {
        auto delta = (Ticks() + current_slice_length) - next_event->cycles;
        current_slice_length -= delta;
    }
    for (auto& core : cores) {
        core.RunSegment(current_slice_length);
        }
}

void SchedulerCore::RunSegment() {
    this->cycles_remaining = root.current_slice_length + delay_cycles;
    delay_cycles = 0;
    do {

        if (Reschedule()) {
            CPU().Run(cycles_remaining);
        }
        if (next_event != root.event_heap.end() && cycles_remaining <= 0) {
            next_event->event_type->callback(0, -cycles_remaining);
        }
        cycles_remaining += deferred_cycles;
    } while (cycles_remaining > 0);
    if (&root.cores[0] == this && root.current_slice_length > delay_cycles) {
        // An optimization: if the first core was interrupted, make that the slice length for the
        // others
        root.current_slice_length -= delay_cycles;
        delay_cycles = 0;
    }
}

void Scheduler::ScheduleEvent(Core::TimingEventType* event, s64 cycles_into_future) {
    ASSERT(cycles_into_future > 0);
    auto& core = cores[current_core_id];

    if ((Ticks() + cycles_into_future) < max_core_time) {
        LOG_WARNING(Core, "Event {} was scheduled {} cycles too soon for all cores to respond",
                    *event->name, max_core_time - (Ticks() + cycles_into_future));
    }

    // Try to continue the current execution:
    if (cycles_into_future < core.cycles_remaining) {
        if (core.AllThreadsIdle()) {
            core.AddTicks(cycles_into_future);
            event->callback(0, 0);
            // If no threads on our core were woken up, it was probably
            // a different core, so end the block now.
            if (!core.Reschedule()) {
                core.EndBlock();
            }
            return;
        }
        // If a thread is running, end the block ASAP so we can run a more
        // precise slice.
        core.EndBlock();
    }

    // Push the resultant event onto the heap
    auto scheduled_time = core.Ticks() + cycles_into_future;
    event_heap.emplace_back(Event{event, scheduled_time});
    std::push_heap(event_heap.begin(), event_heap.end(), std::greater<>());
}

bool SchedulerCore::Reschedule() {
    while (true) {
        if (thread->status == Kernel::ThreadStatus::Running) {
            return true;
        }

        std::make_heap(thread_heap.begin(), thread_heap.end(), std::greater<>());

        auto next_thread = thread_heap.begin();
        if (next_thread == thread_heap.end()) {
            break;
        }

        if (*next_thread == thread) {
            break;
        }

        if ((*next_thread)->status != Kernel::ThreadStatus::Ready) {
            auto next_event = root.event_heap.begin();
            if (next_event == root.event_heap.end()) {
                return;
            }
            if (next_event->cycles > (Ticks() + cycles_remaining)) {
                break;
            }
            auto event = *next_event;
            std::pop_heap(root.event_heap.begin(), root.event_heap.end());
            AddTicks(event.cycles - Ticks());
            event.event_type->callback(0, 0);
            continue;
        }

        CPU().SaveContext(thread->context);
        thread = *next_thread;
        CPU().LoadContext(thread->context);
        return;
    }

    EndBlock();
    return false;
}

void SchedulerCore::EndBlock() {
    CPU().PrepareReschedule();
}

} // namespace Core
