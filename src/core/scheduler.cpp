#include <algorithm>
#include <chrono>
#include <limits>
#include "common/assert.h"
#include "core/core_timing.h"
#include "scheduler.h"

namespace Core {

Thread::Thread(SchedulerCore& core_)
    : core(core_), sequential_id(core_.GetSequentialIndex(this)), Kernel::WaitObject(nullptr) {}

Thread::~Thread() {
    core.RemoveThread(this);
}

void Scheduler::RunSlice() {
    // We assume:
    //  * Threads don't, e.g. write memory at the same time
    //  * Events end with b) a thread waking up, or b) some other change
    //  * Events don't schedule other events to execute immediately

    // Run events scheduled for 'now'. Threshold chosen to reduce small slices.
    while (TimeToNextEvent() < 100) {
        auto next_event = event_heap.front();
        next_event.event_type->callback(0, Ticks() - next_event.cycles);
    }
    // the max slice time is fairly arbitrary, as it represents the largest duration by which cores
    // can be out of sync. The real maximum value without sacrificing accuracy will depend on
    // software.
    current_slice_length = BASE_CLOCK_RATE_ARM11 / 234;
    // Only run the slice til the next event:
    if (TimeToNextEvent() < current_slice_length) {
        current_slice_length = TimeToNextEvent();
    }
    // No cores have run yet, so it's OK to schedule any events we want during this segment
    max_core_time = 0;
    for (auto& core : cores) {
        s64 cycles_run = core.RunSegment(current_slice_length);
        // Uniquely for the first core, we cut the slice if an event was scheduled before the
        // segment's end. This allows other cores to respond to events scheduled on it. For
        // subsequent cores, however, this isn't practical because core 0 (at least) will be ahead
        // in time already.
        // TODO: Implement core re-ordering
        if (core.CanCutSlice()) {
            current_slice_length = std::min(current_slice_length, cycles_run);
            max_core_time = cycles_run + Ticks();
        }
    }
}

s64 Scheduler::TimeToNextEvent() const {
    if (event_heap.empty()) {
        return std::numeric_limits<s64>::max;
    }
    return event_heap.front().cycles - Ticks();
}

Cycles SchedulerCore::RunSegment(Cycles instruction_count) {
    cycles_remaining = instruction_count;
    Cycles defer_cycles = 0;
    do {
        if (Reschedule()) {
            // TODO: Change CPU interface
            CPU().Run(cycles_remaining);
        } else {
            // If idle, just advance time by the requested cycle count
            cycles_remaining = 0;
        }
        // Un-defer any cycles from the last sub-segment
        cycles_remaining += defer_cycles;
        defer_cycles = 0;

        // A block can end early for the following reasons:
        //  * An event was scheduled while idle, and executing it didn't cause our core to continue
        //  * An event was scheduled while running
        //  * All threads have yielded and none are ready to run
        auto time_to_event = root.TimeToNextEvent();
        if (CanCutSlice()) {
            // If we can cut the slice, we should do so at this point.
            cycles_remaining = std::min(time_to_event, cycles_remaining);
        } else if (time_to_event < cycles_remaining) {
            // Event was scheduled by this core, very soon, and we can't cut the slice.
            // So we need to schedule a smaller segment
            defer_cycles = cycles_remaining - time_to_event;
            cycles_remaining = time_to_event;
        }

    } while (cycles_remaining > 0);
}

void Scheduler::ScheduleEvent(Core::TimingEventType* event, Cycles cycles_into_future) {
    ASSERT(cycles_into_future > 0);

    if ((Ticks() + cycles_into_future) < max_core_time) {
        LOG_WARNING(Core, "Event {} was scheduled {} cycles too soon for all cores to respond",
                    *event->name, max_core_time - (Ticks() + cycles_into_future));
    }

    // Try to continue the current execution:
    auto& core = cores[current_core_id];
    if (cycles_into_future < core.cycles_remaining) {
        // If we're idle, try executing the event now to see if it'll wake us up
        if (core.AllThreadsIdle()) {
            core.AddTicks(cycles_into_future);
            event->callback(0, 0);
            core.Reschedule();
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
        // The 3ds uses cooperative multithreading. Never interrupt a thread that's already running.
        if (thread->status == Kernel::ThreadStatus::Running) {
            return true;
        }

        // Order the threads by status and priority
        std::make_heap(thread_heap.begin(), thread_heap.end(), std::greater<>());

        // If there are no threads, end the block
        auto next_thread = thread_heap.begin();
        if (next_thread == thread_heap.end()) {
            break;
        }

        // If there was no change, we're good!
        if (*next_thread == thread) {
            return true;
        }

        // If the next candidate thread isn't ready, try executing events to wake it up
        if ((*next_thread)->status != Kernel::ThreadStatus::Ready) {
            auto next_event = root.event_heap.begin();
            if (next_event == root.event_heap.end()) {
                break;
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

        // Now that we've found us a thread to run, we can change the CPU context inline
        CPU().SaveContext(thread->context);
        thread = *next_thread;
        CPU().LoadContext(thread->context);
        return true;
    }

    // Otherwise, end the block now as there's nothing for the CPU to do.
    EndBlock();
    return false;
}

void SchedulerCore::EndBlock() {
    CPU().PrepareReschedule();
}

void SchedulerCore::WaitOne(Kernel::WaitObject* object) {
    ASSERT(thread->status == Thread::Status::Running);
    if (!object->ShouldWait(thread)) {
        object->Acquire(thread);
        // TODO: Sync result
        return;
    }

    thread->waiting_on = {object};
    object->AddWaitingThread(Kernel::SharedFrom(thread));
    thread->status = Thread::Status::Waiting;
    Reschedule();
    return;
}

void SchedulerCore::WaitAny(std::vector<Kernel::WaitObject*> objects) {
    ASSERT(thread->status == Thread::Status::Running);
    auto active_obj =
        std::find_if(objects.begin(), objects.end(),
                     [this](const Kernel::WaitObject* obj) { return !obj->ShouldWait(thread); });
    if (active_obj != objects.end()) {
        (*active_obj)->Acquire(thread);
        // TODO: Sync result
        return;
    }

    thread->waiting_on = objects;
    for (auto obj : objects) {
        obj->AddWaitingThread(Kernel::SharedFrom(thread));
    }
    thread->status = Thread::Status::Waiting;
    Reschedule();
    return;
}

void SchedulerCore::WaitAll(std::vector<Kernel::WaitObject*> objects) {}

void SchedulerCore::Sleep() {
    ASSERT(thread->status == Thread::Status::Running);
    thread->status == Thread::Status::Waiting;
    Reschedule();
}

void SchedulerCore::Sleep(std::chrono::nanoseconds duration) {
    ASSERT(thread->status == Thread::Status::Running);
    if (duration.count() > 0) {
        root.ScheduleEvent(WakeupEvent, duration);
        thread->status == Thread::Status::Waiting;
    } else {
        thread->status == Thread::Status::Yielding;
    }
    Reschedule();
}

void Thread::WaitObjectReady(Kernel::WaitObject* object) {
    if (std::remove(waiting_on.begin(), waiting_on.end(), object) != waiting_on.end()) {
        if (wait_all) {
            if (waiting_on.empty()) {
                status == Thread::Status::Ready;
            }
        } else {
            waiting_on.clear();
            status == Thread::Status::Ready;
        }
    }
}

} // namespace Core
