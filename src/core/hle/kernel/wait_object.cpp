// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include "common/archives.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

namespace Kernel {

template <class Archive>
void WaitObject::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<Object>(*this);
    ar& waiting_threads;
    // NB: hle_notifier *not* serialized since it's a callback!
    // Fortunately it's only used in one place (DSP) so we can reconstruct it there
}
SERIALIZE_IMPL(WaitObject)

void WaitObject::AddWaitingThread(std::shared_ptr<Thread> thread) {
    auto itr = std::find(waiting_threads.begin(), waiting_threads.end(), thread);
    if (itr == waiting_threads.end())
        waiting_threads.push_back(std::move(thread));
}

void WaitObject::RemoveWaitingThread(Thread* thread) {
    auto itr = std::find_if(waiting_threads.begin(), waiting_threads.end(),
                            [thread](const auto& p) { return p.get() == thread; });
    // If a thread passed multiple handles to the same object,
    // the kernel might attempt to remove the thread from the object's
    // waiting threads list multiple times.
    if (itr != waiting_threads.end())
        waiting_threads.erase(itr);
}

std::shared_ptr<Thread> WaitObject::GetHighestPriorityReadyThread() const {

    std::sort(waiting_threads.begin(), waiting_threads.end());
    auto it = std::find_if(
        waiting_threads.begin(), waiting_threads.end(),
        [this](const std::shared_ptr<Thread>& thread) { return thread->IsWokenBy(this); });
    if (it != waiting_threads.end()) {
        return *it;
    } else {
        return nullptr;
    }
}

void WaitObject::NotifyAvailable() {
    while (auto thread = GetHighestPriorityReadyThread()) {
        thread->WakeFromWaiting(this);
    }

    if (hle_notifier)
        hle_notifier();
}

const std::vector<std::shared_ptr<Thread>>& WaitObject::GetWaitingThreads() const {
    return waiting_threads;
}

void WaitObject::SetHLENotifier(std::function<void()> callback) {
    hle_notifier = std::move(callback);
}

} // namespace Kernel
