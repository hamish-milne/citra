// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/archives.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/global.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Kernel namespace

SERIALIZE_EXPORT_IMPL(Kernel::AddressArbiter)

namespace Kernel {

void AddressArbiter::WaitThread(ThreadManager& core, VAddr wait_address,
                                std::optional<nanoseconds> timeout) {
    core.WaitArb(this, wait_address, timeout);
    waiting_threads.emplace_back(core.GetCurrentThread());
}

void AddressArbiter::RemoveWaitingThread(Thread* thread) {
    auto itr = std::find_if(waiting_threads.begin(), waiting_threads.end(),
                            [=](auto& t) { return t.get() == thread; });
    if (itr != waiting_threads.end()) {
        waiting_threads.erase(itr);
    }
}

void AddressArbiter::ResumeAllThreads(VAddr address) {
    // Determine which threads are waiting on this address, those should be woken up.
    auto itr = std::stable_partition(
        waiting_threads.begin(), waiting_threads.end(),
        [address](const auto& thread) { return !thread->IsWaitingOn(address); });

    // Wake up all the found threads
    std::for_each(itr, waiting_threads.end(), [](auto& thread) { thread->WakeUp(); });

    // Remove the woken up threads from the wait list.
    waiting_threads.erase(itr, waiting_threads.end());
}

std::shared_ptr<Thread> AddressArbiter::ResumeHighestPriorityThread(VAddr address) {
    // Determine which threads are waiting on this address, those should be considered for wakeup.
    auto matches_start = std::stable_partition(
        waiting_threads.begin(), waiting_threads.end(),
        [address](const auto& thread) { return !thread->IsWaitingOn(address); });

    // Iterate through threads, find highest priority thread that is waiting to be arbitrated.
    // Note: The real kernel will pick the first thread in the list if more than one have the
    // same highest priority value. Lower priority values mean higher priority.
    auto itr = std::min_element(matches_start, waiting_threads.end(), [](auto& lhs, auto& rhs) {
        return lhs->GetPriority() < rhs->GetPriority();
    });

    if (itr == waiting_threads.end())
        return nullptr;

    auto thread = *itr;
    thread->WakeUp();

    waiting_threads.erase(itr);
    return thread;
}

AddressArbiter::AddressArbiter(KernelSystem& kernel) : Object(kernel), kernel(kernel) {}
AddressArbiter::~AddressArbiter() {}

std::shared_ptr<AddressArbiter> KernelSystem::CreateAddressArbiter(std::string name) {
    auto address_arbiter{std::make_shared<AddressArbiter>(*this)};

    address_arbiter->name = std::move(name);

    return address_arbiter;
}

// void AddressArbiter::WakeUp(Thread::WakeupReason reason, std::shared_ptr<Thread> thread,
//                             std::shared_ptr<WaitObject> object) {
//     ASSERT(reason == Thread::WakeupReason::Timeout);
//     // Remove the newly-awakened thread from the Arbiter's waiting list.
//     waiting_threads.erase(std::remove(waiting_threads.begin(), waiting_threads.end(), thread),
//                           waiting_threads.end());
// };

ResultCode AddressArbiter::ArbitrateAddress(ThreadManager& core, ArbitrationType type,
                                            VAddr address, s32 value, u64 timeout) {

    auto timeout_callback = std::dynamic_pointer_cast<WakeupCallback>(shared_from_this());

    switch (type) {

    // Signal thread(s) waiting for arbitrate address...
    case ArbitrationType::Signal:
        // Negative value means resume all threads
        if (value < 0) {
            ResumeAllThreads(address);
        } else {
            // Resume first N threads
            for (int i = 0; i < value; i++)
                ResumeHighestPriorityThread(address);
        }
        break;

    // Wait current thread (acquire the arbiter)...
    case ArbitrationType::WaitIfLessThan:
        if ((s32)kernel.memory.Read32(address) < value) {
            WaitThread(core, address, std::nullopt);
        }
        break;
    case ArbitrationType::WaitIfLessThanWithTimeout:
        if ((s32)kernel.memory.Read32(address) < value) {
            WaitThread(core, address, nanoseconds(timeout));
        }
        break;
    case ArbitrationType::DecrementAndWaitIfLessThan: {
        s32 memory_value = kernel.memory.Read32(address);
        if (memory_value < value) {
            // Only change the memory value if the thread should wait
            kernel.memory.Write32(address, (s32)memory_value - 1);
            WaitThread(core, address, std::nullopt);
        }
        break;
    }
    case ArbitrationType::DecrementAndWaitIfLessThanWithTimeout: {
        s32 memory_value = kernel.memory.Read32(address);
        if (memory_value < value) {
            // Only change the memory value if the thread should wait
            kernel.memory.Write32(address, (s32)memory_value - 1);
            WaitThread(core, address, nanoseconds(timeout));
        }
        break;
    }

    default:
        LOG_ERROR(Kernel, "unknown type={}", static_cast<u32>(type));
        return ERR_INVALID_ENUM_VALUE_FND;
    }

    // The calls that use a timeout seem to always return a Timeout error even if they did not put
    // the thread to sleep
    if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
        type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {

        return RESULT_TIMEOUT;
    }
    return RESULT_SUCCESS;
}

} // namespace Kernel
