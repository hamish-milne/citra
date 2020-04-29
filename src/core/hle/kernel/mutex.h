// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include "common/common_types.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/wait_object.h"
#include "core/hle/result.h"

namespace Kernel {

class Thread;

class Mutex final : public WaitObject {
public:
    explicit Mutex(KernelSystem& kernel);
    ~Mutex() override;

    std::string GetTypeName() const override {
        return "Mutex";
    }
    std::string GetName() const override {
        return name;
    }

    static constexpr HandleType HANDLE_TYPE = HandleType::Mutex;
    HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    int lock_count;                         ///< Number of times the mutex has been acquired
    std::string name;                       ///< Name of mutex (optional)
    std::shared_ptr<Thread> holding_thread; ///< Thread that has acquired the mutex

    // void AddWaitingThread(std::shared_ptr<Thread> thread) override;
    // void RemoveWaitingThread(Thread* thread) override;

    bool AcquireOrWait(std::shared_ptr<Thread> thread) override;

    /**
     * Attempts to release the mutex from the specified thread.
     * @param thread Thread that wants to release the mutex.
     * @returns The result code of the operation.
     */
    ResultCode Release(Thread* thread);

protected:
    bool ShouldWait(const Thread* thread) const override;
    void Acquire(Thread* thread) override;

private:
    KernelSystem& kernel;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version) {
        ar& boost::serialization::base_object<WaitObject>(*this);
        ar& lock_count;
        ar& name;
        ar& holding_thread;
    }
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::Mutex)
CONSTRUCT_KERNEL_OBJECT(Kernel::Mutex)
