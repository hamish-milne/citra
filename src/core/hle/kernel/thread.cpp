// Copyright 2014 Citra Emulator Project / PPSSPP Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <list>
#include <unordered_map>
#include <vector>
#include <boost/serialization/string.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/serialization/boost_flat_set.h"
#include "core/arm/arm_interface.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/mutex.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/result.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::Thread)

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
    KernelSystem& kernel, std::string name, VAddr entry_point, u32 priority, u32 arg,
    VAddr stack_top, std::shared_ptr<Kernel::Process> owner_process) {
    auto tls_address = AllocateTLS(kernel, *owner_process);
    auto thread = new Thread(*this, std::move(name), std::move(owner_process), tls_address);
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
    Stop();
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

    // Sort the thread list to find the highest priority ready one
    std::sort(threads.begin(), threads.end());

    // If there are no threads ready, end the block
    auto next_thread = threads.begin();
    if (next_thread == threads.end() || (*next_thread)->status != Thread::Status::Ready) {
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
    return std::find_if(threads.begin(), threads.end(), [](const Kernel::Thread* thread) {
        return thread->status <= Thread::Status::Ready;
    });
}

void ThreadManager::EndBlock() {
    CPU().PrepareReschedule();
}

void ThreadManager::RemoveThread(Kernel::Thread* thread) {}

ResultCode ThreadManager::WaitOne(Kernel::WaitObject* object, std::chrono::nanoseconds timeout) {
    ASSERT(thread->status == Thread::Status::Running);

    // Check if we can sync immediately
    if (!object->ShouldWait(thread)) {
        object->Acquire(thread);
        thread->context->SetCpuRegister(0, RESULT_SUCCESS.raw);
        return RESULT_SUCCESS;
    }

    // Schedule the timeout
    if (timeout.count() == 0) {
        return RESULT_TIMEOUT;
    } else {
        root.ScheduleEvent(thread->wakeup_event.get(), timeout);
    }

    // Wait on the object
    thread->waiting_on = {object};
    object->AddWaitingThread(Kernel::SharedFrom(thread));
    thread->status = Thread::Status::WaitSyncAll;
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
    if (timeout.count() == 0) {
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
    thread->status = Thread::Status::WaitSyncAny;
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
    if (timeout.count() == 0) {
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
    thread->status = Thread::Status::WaitSyncAll;
    Reschedule();
    return RESULT_SUCCESS;
}

void ThreadManager::Sleep() {
    ASSERT(thread->status == Thread::Status::Running);
    thread->status = Thread::Status::WaitSleep;
    Reschedule();
}

void ThreadManager::Sleep(std::chrono::nanoseconds duration) {
    ASSERT(thread->status == Thread::Status::Running);
    if (duration.count() > 0) {
        root.ScheduleEvent(thread->wakeup_event.get(), duration);
        thread->status = Thread::Status::WaitSleep;
    } else {
        thread->status = Thread::Status::Ready;
    }
    Reschedule();
}

void ThreadManager::Stop() {
    ASSERT(thread->status == Thread::Status::Running);
    thread->Stop();
}

void Thread::Stop() {
    if (status != Thread::Status::Destroyed) {
        status = Thread::Status::Destroyed;

        for (auto mutex : held_mutexes) {
            mutex->Release(this);
        }
        held_mutexes.clear();

        core.threads.erase(std::remove(core.threads.begin(), core.threads.end(), this),
                           core.threads.end());

        // Mark the TLS slot in the thread's page as free.
        u32 tls_page = (tls_address - Memory::TLS_AREA_VADDR) / Memory::PAGE_SIZE;
        u32 tls_slot =
            ((tls_address - Memory::TLS_AREA_VADDR) % Memory::PAGE_SIZE) / Memory::TLS_ENTRY_SIZE;
        process->tls_slots[tls_page].reset(tls_slot);
    }
}

bool Thread::IsWokenBy(const Kernel::WaitObject* object) {

    if (status == Thread::Status::WaitSyncAny) {
        return std::find(waiting_on.begin(), waiting_on.end(), object) != waiting_on.end();
    } else if (status == Thread::Status::WaitSyncAll) {
        return std::find_if(waiting_on.begin(), waiting_on.end(),
                            [&](const Kernel::WaitObject* obj) {
                                return obj == object || !obj->ShouldWait(this);
                            }) == waiting_on.end();
    } else {
        return false;
    }
}

void Thread::ResumeFromWait(Kernel::WaitObject* object) {
    if (status == Status::WaitSyncAll) {
        for (auto obj : waiting_on) {
            obj->Acquire(this);
        }
    } else if (status == Status::WaitSyncAny) {
        auto it = std::find(waiting_on.begin(), waiting_on.end(), object);
        auto idx = it - waiting_on.begin();
        object->Acquire(this);
        context->SetCpuRegister(1, static_cast<u32>(idx));
    } else {
        ASSERT(false);
    }
    context->SetCpuRegister(0, RESULT_SUCCESS.raw);

    // Clear waiting objects/threads
    for (auto obj : waiting_on) {
        obj->RemoveWaitingThread(this);
    }
    waiting_on.clear();
    core.root.UnscheduleEvent(wakeup_event.get());
    status = Thread::Status::Ready;
}

void Thread::SetPriority(u32 priority) {
    nominal_priority = priority;
    for (auto obj : waiting_on) {
        auto mutex = dynamic_cast<Kernel::Mutex*>(obj);
        if (mutex && mutex->holding_thread) {
            mutex->holding_thread->real_priority.reset();
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
    }
}

void Thread::OnAcquireMutex(Kernel::Mutex* mutex) {
    for (auto waiting_thread : mutex->GetWaitingThreads()) {
        waiting_thread->real_priority.reset();
    }
    held_mutexes.insert(mutex);
}

void Thread::OnReleaseMutex(Kernel::Mutex* mutex) {
    held_mutexes.erase(mutex);
    for (auto waiting_thread : mutex->GetWaitingThreads()) {
        waiting_thread->real_priority.reset();
    }
}

u32 Thread::GetPriority() {
    if (!real_priority.has_value()) {
        u32 priority = nominal_priority;
        real_priority = priority;
        for (auto mutex : held_mutexes) {
            for (auto waiting_thread : mutex->GetWaitingThreads()) {
                priority = std::min(priority, waiting_thread->GetPriority());
            }
        }
        real_priority = priority;
    }
    return real_priority.value();
}

// template <class Archive>
// void Thread::serialize(Archive& ar, const unsigned int file_version) {
//     ar& boost::serialization::base_object<WaitObject>(*this);
//     ar&* context.get();
//     ar& thread_id;
//     ar& status;
//     ar& entry_point;
//     ar& stack_top;
//     ar& nominal_priority;
//     ar& current_priority;
//     ar& last_running_ticks;
//     ar& processor_id;
//     ar& tls_address;
//     ar& held_mutexes;
//     ar& pending_mutexes;
//     ar& owner_process;
//     ar& wait_objects;
//     ar& wait_address;
//     ar& name;
//     ar& wakeup_callback;
// }

// SERIALIZE_IMPL(Thread)

// bool Thread::ShouldWait(const Thread* thread) const {
//     return status != ThreadStatus::Dead;
// }

// void Thread::Acquire(Thread* thread) {
//     ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
// }

// Thread::Thread(KernelSystem& kernel, u32 core_id)
//     : WaitObject(kernel), context(kernel.GetThreadManager(core_id).NewContext()),
//     core_id(core_id),
//       thread_manager(kernel.GetThreadManager(core_id)) {}
// Thread::~Thread() {}

// Thread* ThreadManager::GetCurrentThread() const {
//     return current_thread.get();
// }

// void Thread::Stop() {
//     // Cancel any outstanding wakeup events for this thread
//     thread_manager.kernel.timing.UnscheduleEvent(thread_manager.ThreadWakeupEventType,
//     thread_id); thread_manager.wakeup_callback_table.erase(thread_id);

//     // Clean up thread from ready queue
//     // This is only needed when the thread is termintated forcefully (SVC TerminateProcess)
//     if (status == ThreadStatus::Ready) {
//         thread_manager.ready_queue.remove(current_priority, this);
//     }

//     status = ThreadStatus::Dead;

//     WakeupAllWaitingThreads();

//     // Clean up any dangling references in objects that this thread was waiting for
//     for (auto& wait_object : wait_objects) {
//         wait_object->RemoveWaitingThread(this);
//     }
//     wait_objects.clear();

//     // Release all the mutexes that this thread holds
//     ReleaseThreadMutexes(this);

//     // Mark the TLS slot in the thread's page as free.
//     u32 tls_page = (tls_address - Memory::TLS_AREA_VADDR) / Memory::PAGE_SIZE;
//     u32 tls_slot =
//         ((tls_address - Memory::TLS_AREA_VADDR) % Memory::PAGE_SIZE) / Memory::TLS_ENTRY_SIZE;
//     owner_process->tls_slots[tls_page].reset(tls_slot);
// }

// void ThreadManager::SwitchContext(Thread* new_thread) {
//     Thread* previous_thread = GetCurrentThread();
//     std::shared_ptr<Process> previous_process = nullptr;

//     Core::Timing& timing = kernel.timing;

//     // Save context for previous thread
//     if (previous_thread) {
//         previous_process = previous_thread->owner_process;
//         previous_thread->last_running_ticks = timing.GetGlobalTicks();
//         cpu->SaveContext(previous_thread->context);

//         if (previous_thread->status == ThreadStatus::Running) {
//             // This is only the case when a reschedule is triggered without the current thread
//             // yielding execution (i.e. an event triggered, system core time-sliced, etc)
//             ready_queue.push_front(previous_thread->current_priority, previous_thread);
//             previous_thread->status = ThreadStatus::Ready;
//         }
//     }

//     // Load context of new thread
//     if (new_thread) {
//         ASSERT_MSG(new_thread->status == ThreadStatus::Ready,
//                    "Thread must be ready to become running.");

//         // Cancel any outstanding wakeup events for this thread
//         timing.UnscheduleEvent(ThreadWakeupEventType, new_thread->thread_id);

//         current_thread = SharedFrom(new_thread);

//         ready_queue.remove(new_thread->current_priority, new_thread);
//         new_thread->status = ThreadStatus::Running;

//         if (previous_process != current_thread->owner_process) {
//             kernel.SetCurrentProcessForCPU(current_thread->owner_process, cpu->GetID());
//         }

//         cpu->LoadContext(new_thread->context);
//         cpu->SetCP15Register(CP15_THREAD_URO, new_thread->GetTLSAddress());
//     } else {
//         current_thread = nullptr;
//         // Note: We do not reset the current process and current page table when idling because
//         // technically we haven't changed processes, our threads are just paused.
//     }
// }

// Thread* ThreadManager::PopNextReadyThread() {
//     Thread* next = nullptr;
//     Thread* thread = GetCurrentThread();

//     if (thread && thread->status == ThreadStatus::Running) {
//         // We have to do better than the current thread.
//         // This call returns null when that's not possible.
//         next = ready_queue.pop_first_better(thread->current_priority);
//         if (!next) {
//             // Otherwise just keep going with the current thread
//             next = thread;
//         }
//     } else {
//         next = ready_queue.pop_first();
//     }

//     return next;
// }

// void ThreadManager::WaitCurrentThread_Sleep() {
//     Thread* thread = GetCurrentThread();
//     thread->status = ThreadStatus::WaitSleep;
// }

// void ThreadManager::ExitCurrentThread() {
//     Thread* thread = GetCurrentThread();
//     thread->Stop();
//     thread_list.erase(std::remove_if(thread_list.begin(), thread_list.end(),
//                                      [thread](const auto& p) { return p.get() == thread; }),
//                       thread_list.end());
// }

// void ThreadManager::ThreadWakeupCallback(u64 thread_id, s64 cycles_late) {
//     std::shared_ptr<Thread> thread = SharedFrom(wakeup_callback_table.at(thread_id));
//     if (thread == nullptr) {
//         LOG_CRITICAL(Kernel, "Callback fired for invalid thread {:08X}", thread_id);
//         return;
//     }

//     if (thread->status == ThreadStatus::WaitSynchAny ||
//         thread->status == ThreadStatus::WaitSynchAll || thread->status == ThreadStatus::WaitArb
//         || thread->status == ThreadStatus::WaitHleEvent) {

//         // Invoke the wakeup callback before clearing the wait objects
//         if (thread->wakeup_callback)
//             thread->wakeup_callback->WakeUp(ThreadWakeupReason::Timeout, thread, nullptr);

//         // Remove the thread from each of its waiting objects' waitlists
//         for (auto& object : thread->wait_objects)
//             object->RemoveWaitingThread(thread.get());
//         thread->wait_objects.clear();
//     }

//     thread->ResumeFromWait();
// }

// void Thread::WakeAfterDelay(s64 nanoseconds) {
//     // Don't schedule a wakeup if the thread wants to wait forever
//     if (nanoseconds == -1)
//         return;

//     thread_manager.kernel.timing.ScheduleEvent(nsToCycles(nanoseconds),
//                                                thread_manager.ThreadWakeupEventType, thread_id);
// }

// void Thread::ResumeFromWait() {
//     ASSERT_MSG(wait_objects.empty(), "Thread is waking up while waiting for objects");

//     switch (status) {
//     case ThreadStatus::WaitSynchAll:
//     case ThreadStatus::WaitSynchAny:
//     case ThreadStatus::WaitHleEvent:
//     case ThreadStatus::WaitArb:
//     case ThreadStatus::WaitSleep:
//     case ThreadStatus::WaitIPC:
//     case ThreadStatus::Dormant:
//         break;

//     case ThreadStatus::Ready:
//         // The thread's wakeup callback must have already been cleared when the thread was first
//         // awoken.
//         ASSERT(wakeup_callback == nullptr);
//         // If the thread is waiting on multiple wait objects, it might be awoken more than once
//         // before actually resuming. We can ignore subsequent wakeups if the thread status has
//         // already been set to ThreadStatus::Ready.
//         return;

//     case ThreadStatus::Running:
//         DEBUG_ASSERT_MSG(false, "Thread with object id {} has already resumed.", GetObjectId());
//         return;
//     case ThreadStatus::Dead:
//         // This should never happen, as threads must complete before being stopped.
//         DEBUG_ASSERT_MSG(false, "Thread with object id {} cannot be resumed because it's DEAD.",
//                          GetObjectId());
//         return;
//     }

//     wakeup_callback = nullptr;

//     thread_manager.ready_queue.push_back(current_priority, this);
//     status = ThreadStatus::Ready;
//     thread_manager.kernel.PrepareReschedule();
// }

// void ThreadManager::DebugThreadQueue() {
//     Thread* thread = GetCurrentThread();
//     if (!thread) {
//         LOG_DEBUG(Kernel, "Current: NO CURRENT THREAD");
//     } else {
//         LOG_DEBUG(Kernel, "0x{:02X} {} (current)", thread->current_priority,
//                   GetCurrentThread()->GetObjectId());
//     }

//     for (auto& t : thread_list) {
//         u32 priority = ready_queue.contains(t.get());
//         if (priority != -1) {
//             LOG_DEBUG(Kernel, "0x{:02X} {}", priority, t->GetObjectId());
//         }
//     }
// }

// /**
//  * Finds a free location for the TLS section of a thread.
//  * @param tls_slots The TLS page array of the thread's owner process.
//  * Returns a tuple of (page, slot, alloc_needed) where:
//  * page: The index of the first allocated TLS page that has free slots.
//  * slot: The index of the first free slot in the indicated page.
//  * alloc_needed: Whether there's a need to allocate a new TLS page (All pages are full).
//  */
// static std::tuple<std::size_t, std::size_t, bool> GetFreeThreadLocalSlot(
//     const std::vector<std::bitset<8>>& tls_slots) {
//     // Iterate over all the allocated pages, and try to find one where not all slots are used.
//     for (std::size_t page = 0; page < tls_slots.size(); ++page) {
//         const auto& page_tls_slots = tls_slots[page];
//         if (!page_tls_slots.all()) {
//             // We found a page with at least one free slot, find which slot it is
//             for (std::size_t slot = 0; slot < page_tls_slots.size(); ++slot) {
//                 if (!page_tls_slots.test(slot)) {
//                     return std::make_tuple(page, slot, false);
//                 }
//             }
//         }
//     }

//     return std::make_tuple(0, 0, true);
// }

// /**
//  * Resets a thread context, making it ready to be scheduled and run by the CPU
//  * @param context Thread context to reset
//  * @param stack_top Address of the top of the stack
//  * @param entry_point Address of entry point for execution
//  * @param arg User argument for thread
//  */
// static void ResetThreadContext(const std::unique_ptr<ARM_Interface::ThreadContext>& context,
//                                u32 stack_top, u32 entry_point, u32 arg) {
//     context->Reset();
//     context->SetCpuRegister(0, arg);
//     context->SetProgramCounter(entry_point);
//     context->SetStackPointer(stack_top);
//     context->SetCpsr(USER32MODE | ((entry_point & 1) << 5)); // Usermode and THUMB mode
// }

// ResultVal<std::shared_ptr<Thread>> KernelSystem::CreateThread(
//     std::string name, VAddr entry_point, u32 priority, u32 arg, s32 processor_id, VAddr
//     stack_top, std::shared_ptr<Process> owner_process) {
//     // Check if priority is in ranged. Lowest priority -> highest priority id.
//     if (priority > ThreadPrioLowest) {
//         LOG_ERROR(Kernel_SVC, "Invalid thread priority: {}", priority);
//         return ERR_OUT_OF_RANGE;
//     }

//     if (processor_id > ThreadProcessorIdMax) {
//         LOG_ERROR(Kernel_SVC, "Invalid processor id: {}", processor_id);
//         return ERR_OUT_OF_RANGE_KERNEL;
//     }

//     // TODO(yuriks): Other checks, returning 0xD9001BEA

//     if (!Memory::IsValidVirtualAddress(*owner_process, entry_point)) {
//         LOG_ERROR(Kernel_SVC, "(name={}): invalid entry {:08x}", name, entry_point);
//         // TODO: Verify error
//         return ResultCode(ErrorDescription::InvalidAddress, ErrorModule::Kernel,
//                           ErrorSummary::InvalidArgument, ErrorLevel::Permanent);
//     }

//     auto thread{std::make_shared<Thread>(*this, processor_id)};

//     thread_managers[processor_id]->thread_list.push_back(thread);
//     thread_managers[processor_id]->ready_queue.prepare(priority);

//     thread->thread_id = NewThreadId();
//     thread->status = ThreadStatus::Dormant;
//     thread->entry_point = entry_point;
//     thread->stack_top = stack_top;
//     thread->nominal_priority = thread->current_priority = priority;
//     thread->last_running_ticks = timing.GetGlobalTicks();
//     thread->processor_id = processor_id;
//     thread->wait_objects.clear();
//     thread->wait_address = 0;
//     thread->name = std::move(name);
//     thread_managers[processor_id]->wakeup_callback_table[thread->thread_id] = thread.get();
//     thread->owner_process = owner_process;

//     // Find the next available TLS index, and mark it as used
//     auto& tls_slots = owner_process->tls_slots;

//     auto [available_page, available_slot, needs_allocation] = GetFreeThreadLocalSlot(tls_slots);

//     if (needs_allocation) {
//         // There are no already-allocated pages with free slots, lets allocate a new one.
//         // TLS pages are allocated from the BASE region in the linear heap.
//         auto memory_region = GetMemoryRegion(MemoryRegion::BASE);

//         // Allocate some memory from the end of the linear heap for this region.
//         auto offset = memory_region->LinearAllocate(Memory::PAGE_SIZE);
//         if (!offset) {
//             LOG_ERROR(Kernel_SVC,
//                       "Not enough space in region to allocate a new TLS page for thread");
//             return ERR_OUT_OF_MEMORY;
//         }
//         owner_process->memory_used += Memory::PAGE_SIZE;

//         tls_slots.emplace_back(0); // The page is completely available at the start
//         available_page = tls_slots.size() - 1;
//         available_slot = 0; // Use the first slot in the new page

//         auto& vm_manager = owner_process->vm_manager;

//         // Map the page to the current process' address space.
//         vm_manager.MapBackingMemory(Memory::TLS_AREA_VADDR + available_page * Memory::PAGE_SIZE,
//                                     memory.GetFCRAMRef(*offset), Memory::PAGE_SIZE,
//                                     MemoryState::Locked);
//     }

//     // Mark the slot as used
//     tls_slots[available_page].set(available_slot);
//     thread->tls_address = Memory::TLS_AREA_VADDR + available_page * Memory::PAGE_SIZE +
//                           available_slot * Memory::TLS_ENTRY_SIZE;

//     memory.ZeroBlock(*owner_process, thread->tls_address, Memory::TLS_ENTRY_SIZE);

//     // TODO(peachum): move to ScheduleThread() when scheduler is added so selected core is used
//     // to initialize the context
//     ResetThreadContext(thread->context, stack_top, entry_point, arg);

//     thread_managers[processor_id]->ready_queue.push_back(thread->current_priority, thread.get());
//     thread->status = ThreadStatus::Ready;

//     return MakeResult<std::shared_ptr<Thread>>(std::move(thread));
// }

// void Thread::SetPriority(u32 priority) {
//     ASSERT_MSG(priority <= ThreadPrioLowest && priority >= ThreadPrioHighest,
//                "Invalid priority value.");
//     // If thread was ready, adjust queues
//     if (status == ThreadStatus::Ready)
//         thread_manager.ready_queue.move(this, current_priority, priority);
//     else
//         thread_manager.ready_queue.prepare(priority);

//     nominal_priority = current_priority = priority;
// }

// void Thread::UpdatePriority() {
//     u32 best_priority = nominal_priority;
//     for (auto& mutex : held_mutexes) {
//         if (mutex->priority < best_priority)
//             best_priority = mutex->priority;
//     }
//     BoostPriority(best_priority);
// }

// void Thread::BoostPriority(u32 priority) {
//     // If thread was ready, adjust queues
//     if (status == ThreadStatus::Ready)
//         thread_manager.ready_queue.move(this, current_priority, priority);
//     else
//         thread_manager.ready_queue.prepare(priority);
//     current_priority = priority;
// }

// std::shared_ptr<Thread> SetupMainThread(KernelSystem& kernel, u32 entry_point, u32 priority,
//                                         std::shared_ptr<Process> owner_process) {
//     // Initialize new "main" thread
//     auto thread_res =
//         kernel.CreateThread("main", entry_point, priority, 0, owner_process->ideal_processor,
//                             Memory::HEAP_VADDR_END, owner_process);

//     std::shared_ptr<Thread> thread = std::move(thread_res).Unwrap();

//     thread->context->SetFpscr(FPSCR_DEFAULT_NAN | FPSCR_FLUSH_TO_ZERO | FPSCR_ROUND_TOZERO |
//                               FPSCR_IXC); // 0x03C00010

//     // Note: The newly created thread will be run when the scheduler fires.
//     return thread;
// }

// bool ThreadManager::HaveReadyThreads() {
//     return ready_queue.get_first() != nullptr;
// }

// void ThreadManager::Reschedule() {
//     Thread* cur = GetCurrentThread();
//     Thread* next = PopNextReadyThread();

//     if (cur && next) {
//         LOG_TRACE(Kernel, "context switch {} -> {}", cur->GetObjectId(), next->GetObjectId());
//     } else if (cur) {
//         LOG_TRACE(Kernel, "context switch {} -> idle", cur->GetObjectId());
//     } else if (next) {
//         LOG_TRACE(Kernel, "context switch idle -> {}", next->GetObjectId());
//     } else {
//         LOG_TRACE(Kernel, "context switch idle -> idle, do nothing");
//         return;
//     }

//     SwitchContext(next);
// }

// void Thread::SetWaitSynchronizationResult(ResultCode result) {
//     context->SetCpuRegister(0, result.raw);
// }

// void Thread::SetWaitSynchronizationOutput(s32 output) {
//     context->SetCpuRegister(1, output);
// }

// s32 Thread::GetWaitObjectIndex(const WaitObject* object) const {
//     ASSERT_MSG(!wait_objects.empty(), "Thread is not waiting for anything");
//     const auto match = std::find_if(wait_objects.rbegin(), wait_objects.rend(),
//                                     [object](const auto& p) { return p.get() == object; });
//     return static_cast<s32>(std::distance(match, wait_objects.rend()) - 1);
// }

// VAddr Thread::GetCommandBufferAddress() const {
//     // Offset from the start of TLS at which the IPC command buffer begins.
//     constexpr u32 command_header_offset = 0x80;
//     return GetTLSAddress() + command_header_offset;
// }

// ThreadManager::ThreadManager(Kernel::KernelSystem& kernel, u32 core_id) : kernel(kernel) {
//     ThreadWakeupEventType = kernel.timing.RegisterEvent(
//         "ThreadWakeupCallback_" + std::to_string(core_id),
//         [this](u64 thread_id, s64 cycle_late) { ThreadWakeupCallback(thread_id, cycle_late); });
// }

// ThreadManager::~ThreadManager() {
//     for (auto& t : thread_list) {
//         t->Stop();
//     }
// }

// const std::vector<std::shared_ptr<Thread>>& ThreadManager::GetThreadList() {
//     return thread_list;
// }

} // namespace Kernel
