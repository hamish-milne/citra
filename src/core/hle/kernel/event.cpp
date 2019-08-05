// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <map>
#include <vector>
#include "common/assert.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "core/savestate/binary_rw.h"

namespace Kernel {

Event::Event(KernelSystem& kernel) : WaitObject(kernel) {}
Event::~Event() {}

std::shared_ptr<Event> KernelSystem::CreateEvent(ResetType reset_type, std::string name) {
    auto evt{std::make_shared<Event>(*this)};

    evt->signaled = false;
    evt->reset_type = reset_type;
    evt->name = std::move(name);

    return evt;
}

bool Event::ShouldWait(const Thread* thread) const {
    return !signaled;
}

void Event::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    if (reset_type == ResetType::OneShot)
        signaled = false;
}

void Event::Signal() {
    signaled = true;
    WakeupAllWaitingThreads();
}

void Event::Clear() {
    signaled = false;
}

void Event::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();

    if (reset_type == ResetType::Pulse)
        signaled = false;
}

void Event::Serialize(std::ostream &stream) const
{
    SaveState::BinaryWriter writer{stream};
    writer.Write(reset_type);
    writer.Write(signaled);
    writer.Write(name);
}

void Event::Deserialize(std::istream &stream)
{
    SaveState::BinaryReader reader{stream};
    reset_type = reader.Read<ResetType>();
    signaled = reader.Read<bool>();
    name = reader.ReadString();
}

} // namespace Kernel
