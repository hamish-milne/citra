#include <algorithm>
#include <chrono>
#include <limits>
#include "common/assert.h"
#include "core/core_timing.h"
#include "core/hle/kernel/errors.h"
#include "scheduler.h"

namespace Core {

Scheduler::Scheduler(u32 core_count, std::function<ARM_Interface*()> constructor) {
    for (u32 i = 0; i < core_count; i++) {
        cores.emplace_back(i, std::unique_ptr<ARM_Interface>(constructor()));
    }
}

void Scheduler::RunSlice() {
    // We assume:
    //  * Threads don't, e.g. write memory at the same time
    //  * Events end with b) a thread waking up, or b) some other change
    //  * Events don't schedule other events to execute immediately

    // Run events scheduled for 'now'. Threshold chosen to reduce small slices.
    while (TimeToNextEvent() < Ticks(100)) {
        auto next_event = events.top();
        next_event.event_type->Execute(next_event.userdata, Time_Current() - next_event.time);
        events.pop();
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

Ticks Scheduler::TimeToNextEvent() const {
    if (events.empty()) {
        return Ticks(std::numeric_limits<s64>::max);
    }
    return events.top().time - Time_LB();
}

void Scheduler::ScheduleEvent(Event* event, Ticks cycles_into_future, u64 userdata) {
    ASSERT(cycles_into_future > Ticks(0));

    if ((Time_Current() + cycles_into_future) < Time_UB()) {
        LOG_WARNING(Core, "Event {} was scheduled {} cycles too soon for all cores to respond",
                    event->Name(), Time_UB() - (Time_Current() + cycles_into_future));
    }

    // Try to continue the current execution:
    auto& core = cores[current_core_id];
    if (Cycles(cycles_into_future, 1.0) < core.cycles_remaining) {
        // If we're idle, try executing the event now to see if it'll wake us up
        if (core.AllThreadsIdle()) {
            core.AddCycles(Cycles(cycles_into_future, 1.0));
            event->Execute(userdata, Ticks(0));
            core.Reschedule();
            return;
        }
        // If a thread is running, end the block ASAP so we can run a more
        // precise slice.
        core.EndBlock();
    }

    // Push the resultant event onto the heap
    auto scheduled_time = Time_Current() + cycles_into_future;
    events.emplace(EventInstance{event, scheduled_time, userdata});
}

} // namespace Core

namespace Kernel {

/**
 * Finds a free location for the TLS section of a thread.
 * @param tls_slots The TLS page array of the thread's owner process.
 * Returns a tuple of (page, slot, alloc_needed) where:
 * page: The index of the first allocated TLS page that has free slots.
 * slot: The index of the first free slot in the indicated page.
 * alloc_needed: Whether there's a need to allocate a new TLS page (All pages are full).
 */
static std::tuple<std::size_t, std::size_t, bool> GetFreeThreadLocalSlot(
    const std::vector<std::bitset<8>>& tls_slots) {
    // Iterate over all the allocated pages, and try to find one where not all slots are used.
    for (std::size_t page = 0; page < tls_slots.size(); ++page) {
        const auto& page_tls_slots = tls_slots[page];
        if (!page_tls_slots.all()) {
            // We found a page with at least one free slot, find which slot it is
            for (std::size_t slot = 0; slot < page_tls_slots.size(); ++slot) {
                if (!page_tls_slots.test(slot)) {
                    return std::make_tuple(page, slot, false);
                }
            }
        }
    }

    return std::make_tuple(0, 0, true);
}

static std::optional<VAddr> AllocateTLS(KernelSystem& kernel, Process& process) {
    // Find the next available TLS index, and mark it as used
    auto& tls_slots = process.tls_slots;

    auto [available_page, available_slot, needs_allocation] = GetFreeThreadLocalSlot(tls_slots);

    if (needs_allocation) {
        // There are no already-allocated pages with free slots, lets allocate a new one.
        // TLS pages are allocated from the BASE region in the linear heap.
        auto memory_region = kernel.GetMemoryRegion(MemoryRegion::BASE);

        // Allocate some memory from the end of the linear heap for this region.
        auto offset = memory_region->LinearAllocate(Memory::PAGE_SIZE);
        if (!offset) {
            LOG_ERROR(Kernel_SVC,
                      "Not enough space in region to allocate a new TLS page for thread");
            return {};
        }
        process.memory_used += Memory::PAGE_SIZE;

        tls_slots.emplace_back(0); // The page is completely available at the start
        available_page = tls_slots.size() - 1;
        available_slot = 0; // Use the first slot in the new page

        auto& vm_manager = process.vm_manager;

        // Map the page to the current process' address space.
        vm_manager.MapBackingMemory(Memory::TLS_AREA_VADDR + available_page * Memory::PAGE_SIZE,
                                    kernel.memory.GetFCRAMRef(*offset), Memory::PAGE_SIZE,
                                    MemoryState::Locked);
    }

    // Mark the slot as used
    tls_slots[available_page].set(available_slot);
    auto tls_address = Memory::TLS_AREA_VADDR + available_page * Memory::PAGE_SIZE +
                       available_slot * Memory::TLS_ENTRY_SIZE;

    kernel.memory.ZeroBlock(process, tls_address, Memory::TLS_ENTRY_SIZE);
    return tls_address;
}

std::shared_ptr<Thread> ThreadManager::CreateThread(
    std::string name, VAddr entry_point, u32 priority, u32 arg, VAddr stack_top,
    std::shared_ptr<Kernel::Process> owner_process) {
    auto tls_address = AllocateTLS(Core::System::GetInstance()->kernel, *owner_process);
    auto thread = new Kernel::Thread(*this, std::move(name), std::move(owner_process), tls_address);
    auto& context = *thread->context;
    context.SetCpuRegister(0, arg);
    context.SetProgramCounter(entry_point);
    context.SetStackPointer(stack_top);
    context.SetCpsr(USER32MODE | ((entry_point & 1) << 5)); // Usermode and THUMB mode
    return {thread};
}

class Thread::WakeupEvent : public Core::Event {
    Thread& parent;

public:
    explicit WakeupEvent(Thread& parent_) : parent(parent_) {}

    const std::string& Name() override {
        return "Thread wakeup";
    }

    void Execute(u64 userdata, Ticks cycles_late) {
        parent.WakeUp();
    }
};

Thread::Thread(ThreadManager& core_, std::string name_, std::shared_ptr<Kernel::Process> process_,
               VAddr tls_address_)
    : Kernel::WaitObject(nullptr), core(core_), name(std::move(name_)),
      wakeup_event(new Thread::WakeupEvent(*this)), process(std::move(process_)),
      context(core.cpu->NewContext()), tls_address(tls_address_) {}

Thread::~Thread() {
    core.RemoveThread(this);

    // Mark the TLS slot in the thread's page as free.
    u32 tls_page = (tls_address - Memory::TLS_AREA_VADDR) / Memory::PAGE_SIZE;
    u32 tls_slot =
        ((tls_address - Memory::TLS_AREA_VADDR) % Memory::PAGE_SIZE) / Memory::TLS_ENTRY_SIZE;
    process->tls_slots[tls_page].reset(tls_slot);
}

Ticks ThreadManager::RunSegment(::Ticks segment_length) {
    cycles_remaining = Cycles(segment_length, 1.0);
    Cycles defer_cycles{0};
    do {
        if (Reschedule()) {
            // TODO: Change CPU interface
            CPU().Run(cycles_remaining);
        } else {
            // If idle, just advance time by the requested cycle count
            cycles_remaining = Cycles(0);
        }
        // Un-defer any cycles from the last sub-segment
        cycles_remaining = cycles_remaining + defer_cycles;
        defer_cycles = Cycles(0);

        // A block can end early for the following reasons:
        //  * An event was scheduled while idle, and executing it didn't cause our core to continue
        //  * An event was scheduled while running
        //  * All threads have yielded and none are ready to run
        auto time_to_event = Cycles(root.TimeToNextEvent(), 1.0);
        if (CanCutSlice()) {
            // If we can cut the slice, we should do so at this point.
            cycles_remaining = std::min(time_to_event, cycles_remaining);
        } else if (time_to_event < cycles_remaining) {
            // Event was scheduled by this core, very soon, and we can't cut the slice.
            // So we need to schedule a smaller segment
            defer_cycles = cycles_remaining - time_to_event;
            cycles_remaining = time_to_event;
        }

    } while (cycles_remaining > Cycles(0));
}

bool ThreadManager::Reschedule() {

    // The 3ds uses cooperative multithreading. Never interrupt a thread that's already running.
    if (thread->status == Thread::Status::Running) {
        return true;
    }

    // If there are no threads ready, end the block
    auto next_thread = thread_heap.begin();
    if (next_thread == thread_heap.end() || (*next_thread)->status != Thread::Status::Ready) {
        EndBlock();
        return false;
    }

    // If there was no change, we're good!
    if (*next_thread == thread) {
        return true;
    }

    // Now that we've found us a thread to run, we can change the CPU context inline
    CPU().SaveContext(thread->context);
    thread = *next_thread;
    CPU().LoadContext(thread->context);
    cpu->SetCP15Register(CP15_THREAD_URO, thread->tls_address);
    return true;
}

bool ThreadManager::AllThreadsIdle() const {
    // TODO: Also check WaitObjects?
    return std::find_if(thread_heap.begin(), thread_heap.end(), [](const Kernel::Thread* thread) {
        return thread->status <= Thread::Status::Ready;
    });
}

void ThreadManager::EndBlock() {
    CPU().PrepareReschedule();
}

void ThreadManager::RemoveThread(Kernel::Thread* thread) {
    std::remove(thread_heap.begin(), thread_heap.end(), thread);
    std::make_heap(thread_heap.begin(), thread_heap.end());
}

void Thread::SetStatus(Thread::Status status) {
    this->status = status;
    std::make_heap(core.thread_heap.begin(), core.thread_heap.end());
}

ResultCode ThreadManager::WaitOne(Kernel::WaitObject* object, std::chrono::nanoseconds timeout) {
    ASSERT(thread->status == Thread::Status::Running);

    // Check if we can sync immediately
    if (!object->ShouldWait(thread)) {
        object->Acquire(thread);
        thread->context->SetCpuRegister(0, RESULT_SUCCESS.raw);
        return RESULT_SUCCESS;
    }

    // Schedule the timeout
    if (timeout.count == 0) {
        return RESULT_TIMEOUT;
    } else {
        root.ScheduleEvent(thread->wakeup_event.get(), timeout);
    }

    // Wait on the object
    thread->waiting_on = {object};
    object->AddWaitingThread(Kernel::SharedFrom(thread));
    thread->SetStatus(Thread::Status::WaitSyncAll);
    Reschedule();
    return RESULT_SUCCESS;
}

ResultCode ThreadManager::WaitAny(std::vector<Kernel::WaitObject*> objects,
                                  std::chrono::nanoseconds timeout) {
    ASSERT(thread->status == Thread::Status::Running);

    // Check if we can sync immediately
    auto active_obj =
        std::find_if(objects.begin(), objects.end(),
                     [this](const Kernel::WaitObject* obj) { return !obj->ShouldWait(thread); });
    if (active_obj != objects.end()) {
        (*active_obj)->Acquire(thread);
        thread->context->SetCpuRegister(0, RESULT_SUCCESS.raw);
        thread->context->SetCpuRegister(1, active_obj - objects.begin());
        return RESULT_SUCCESS;
    }

    // Schedule the timeout
    if (timeout.count == 0) {
        auto inactive_obj =
            std::find_if(objects.begin(), objects.end(),
                         [this](const Kernel::WaitObject* obj) { return obj->ShouldWait(thread); });
        if (inactive_obj != objects.end()) {
            return RESULT_TIMEOUT;
        }
    } else {
        root.ScheduleEvent(thread->wakeup_event.get(), timeout);
    }

    thread->waiting_on = objects;
    for (auto obj : objects) {
        obj->AddWaitingThread(Kernel::SharedFrom(thread));
    }
    thread->SetStatus(Thread::Status::WaitSyncAny);
    Reschedule();
    return RESULT_SUCCESS;
}

ResultCode ThreadManager::WaitAll(std::vector<Kernel::WaitObject*> objects,
                                  std::chrono::nanoseconds timeout) {
    ASSERT(thread->status == Thread::Status::Running);

    // Check if we can sync immediately
    auto inactive_obj =
        std::find_if(objects.begin(), objects.end(),
                     [this](const Kernel::WaitObject* obj) { return obj->ShouldWait(thread); });
    if (inactive_obj == objects.end()) {
        for (auto obj : objects) {
            obj->Acquire(thread);
        }
        thread->context->SetCpuRegister(0, RESULT_SUCCESS.raw);
        return RESULT_SUCCESS;
    }

    // Schedule the timeout
    if (timeout.count == 0) {
        auto active_obj =
            std::find_if(objects.begin(), objects.end(), [this](const Kernel::WaitObject* obj) {
                return !obj->ShouldWait(thread);
            });
        if (active_obj == objects.end()) {
            return RESULT_TIMEOUT;
        }
    } else {
        root.ScheduleEvent(thread->wakeup_event.get(), timeout);
    }

    // Wait on the objects
    thread->waiting_on = objects;
    for (auto obj : objects) {
        obj->AddWaitingThread(Kernel::SharedFrom(thread));
    }
    thread->SetStatus(Thread::Status::WaitSyncAll);
    Reschedule();
    return RESULT_SUCCESS;
}

void ThreadManager::Sleep() {
    ASSERT(thread->status == Thread::Status::Running);
    thread->SetStatus(Thread::Status::WaitSleep);
    Reschedule();
}

void ThreadManager::Sleep(std::chrono::nanoseconds duration) {
    ASSERT(thread->status == Thread::Status::Running);
    if (duration.count() > 0) {
        root.ScheduleEvent(thread->wakeup_event.get(), duration);
        thread->SetStatus(Thread::Status::WaitSleep);
    } else {
        thread->SetStatus(Thread::Status::Ready);
    }
    Reschedule();
}

void ThreadManager::Stop() {
    ASSERT(thread->status == Thread::Status::Running);
    thread->status = Thread::Status::Destroyed;
    RemoveThread(thread);
}

void Thread::WaitObjectReady(Kernel::WaitObject* object) {
    auto it = std::remove(waiting_on.begin(), waiting_on.end(), object);
    if (it != waiting_on.end()) {
        if (status == Status::WaitSyncAll) {
            if (waiting_on.empty()) {
                context->SetCpuRegister(0, RESULT_SUCCESS.raw);
                status = Thread::Status::Ready;
                // TODO: Sort
            }
        } else {
            auto idx = it - waiting_on.begin();
            context->SetCpuRegister(0, RESULT_SUCCESS.raw);
            context->SetCpuRegister(1, static_cast<u32>(idx));
            waiting_on.clear();
            status = Thread::Status::Ready;
            // TODO: Sort
        }
    }
}

void Thread::WakeUp() {
    switch (status) {
    case Status::WaitSyncAny:
        context->SetCpuRegister(1, 0);
    case Status::WaitSyncAll:
        context->SetCpuRegister(0, RESULT_TIMEOUT.raw);
    case Status::WaitSleep:
        status = Thread::Status::Ready;
        // TODO: Sort
    }
}

} // namespace Kernel
