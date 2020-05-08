// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/session.h"
#include "core/hle/kernel/thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::ServerSession)

namespace Kernel {

template <class Archive>
void ServerSession::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<WaitObject>(*this);
    ar& name;
    ar& parent;
    ar& hle_handler;
    ar& pending_requesting_threads;
    ar& currently_handling;
    ar& mapped_buffer_context;
}
SERIALIZE_IMPL(ServerSession)

ServerSession::ServerSession(KernelSystem& kernel) : WaitObject(kernel), kernel(kernel) {}
ServerSession::~ServerSession() {
    // This destructor will be called automatically when the last ServerSession handle is closed by
    // the emulated application.

    // Decrease the port's connection count.
    if (parent->port)
        parent->port->ConnectionClosed();

    // TODO(Subv): Wake up all the ClientSession's waiting threads and set
    // the SendSyncRequest result to 0xC920181A.

    parent->server = nullptr;
}

ResultVal<std::shared_ptr<ServerSession>> ServerSession::Create(KernelSystem& kernel,
                                                                std::string name) {
    auto server_session{std::make_shared<ServerSession>(kernel)};

    server_session->name = std::move(name);
    server_session->parent = nullptr;

    return MakeResult(std::move(server_session));
}

bool ServerSession::ShouldWait(const Thread* thread) const {
    // Closed sessions should never wait, an error will be returned from svcReplyAndReceive.
    if (parent->client == nullptr)
        return false;
    // Wait if we have no pending requests, or if we're currently handling a request.
    return pending_requesting_threads.empty() || currently_handling != nullptr;
}

void ServerSession::Acquire(Thread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    // If the client endpoint was closed, don't do anything. This ServerSession is now useless and
    // will linger until its last handle is closed by the running application.
    if (parent->client == nullptr)
        return;

    // We are now handling a request, pop it from the stack.
    ASSERT(!pending_requesting_threads.empty());
    currently_handling = pending_requesting_threads.back();
    pending_requesting_threads.pop_back();
}

ResultCode ServerSession::ReceiveIPCRequest(Thread* thread) {
    if (parent->client == nullptr) {
        return ERR_SESSION_CLOSED_BY_REMOTE;
    }

    VAddr target_address = thread->GetCommandBufferAddress();
    VAddr source_address = currently_handling->GetCommandBufferAddress();

    ResultCode translation_result =
        TranslateCommandBuffer(kernel, kernel.memory, currently_handling, SharedFrom(thread),
                               source_address, target_address, mapped_buffer_context, false);

    // If a translation error occurred, immediately resume the client thread.
    if (translation_result.IsError()) {
        // Set the output of SendSyncRequest in the client thread to the translation output.
        // currently_handling->SetWaitSynchronizationResult(translation_result);

        // currently_handling->ResumeFromWait();
        // currently_handling = nullptr;

        // TODO(Subv): This path should try to wait again on the same objects.
        ASSERT_MSG(false, "ReplyAndReceive translation error behavior unimplemented");
    }

    return translation_result;
}

ResultCode ServerSession::HandleSyncRequest(std::shared_ptr<Thread> thread) {
    // The ServerSession received a sync request, this means that there's new data available
    // from its ClientSession, so wake up any threads that may be waiting on a svcReplyAndReceive or
    // similar.

    // If this ServerSession has an associated HLE handler, forward the request to it.
    if (hle_handler != nullptr) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH + 2 * IPC::MAX_STATIC_BUFFERS> cmd_buf;
        auto& current_process = thread->Process();
        kernel.memory.ReadBlock(current_process, thread->GetCommandBufferAddress(), cmd_buf.data(),
                                cmd_buf.size() * sizeof(u32));

        auto context =
            std::make_shared<Kernel::HLERequestContext>(kernel, SharedFrom(this), thread);
        context->PopulateFromIncomingCommandBuffer(cmd_buf.data(), SharedFrom(&current_process));

        hle_handler->HandleSyncRequest(*context);

        ASSERT(thread->GetStatus() == Thread::Status::Running ||
               thread->GetStatus() == Thread::Status::WaitSyncAny);
        // Only write the response immediately if the thread is still running. If the HLE handler
        // put the thread to sleep then the writing of the command buffer will be deferred to the
        // wakeup callback.
        if (thread->GetStatus() == Thread::Status::Running) {
            context->WriteToOutgoingCommandBuffer(cmd_buf.data(), current_process);
            kernel.memory.WriteBlock(current_process, thread->GetCommandBufferAddress(),
                                     cmd_buf.data(), cmd_buf.size() * sizeof(u32));
        }
    }

    if (thread->GetStatus() == Thread::Status::Running) {
        // Put the thread to sleep until the server replies, it will be awoken in
        // svcReplyAndReceive for LLE servers.

        if (hle_handler != nullptr) {
            // For HLE services, we put the request threads to sleep for a short duration to
            // simulate IPC overhead, but only if the HLE handler didn't put the thread to sleep for
            // other reasons like an async callback. The IPC overhead is needed to prevent
            // starvation when a thread only does sync requests to HLE services while a
            // lower-priority thread is waiting to run.

            // This delay was approximated in a homebrew application by measuring the average time
            // it takes for svcSendSyncRequest to return when performing the SetLcdForceBlack IPC
            // request to the GSP:GPU service in a n3DS with firmware 11.6. The measured values have
            // a high variance and vary between models.
            static constexpr u64 IPCDelayNanoseconds = 39000;
            thread->Core().Sleep(nanoseconds(IPCDelayNanoseconds));
        } else {
            thread->Core().Sleep();
            // Add the thread to the list of threads that have issued a sync request with this
            // server.
            pending_requesting_threads.push_back(std::move(thread));
        }
    }

    // If this ServerSession does not have an HLE implementation, just wake up the threads waiting
    // on it.
    WakeupAllWaitingThreads();
    return RESULT_SUCCESS;
}

KernelSystem::SessionPair KernelSystem::CreateSessionPair(const std::string& name,
                                                          std::shared_ptr<ClientPort> port) {
    auto server_session = ServerSession::Create(*this, name + "_Server").Unwrap();
    auto client_session{std::make_shared<ClientSession>(*this)};
    client_session->name = name + "_Client";

    std::shared_ptr<Session> parent(new Session);
    parent->client = client_session.get();
    parent->server = server_session.get();
    parent->port = port;

    client_session->parent = parent;
    server_session->parent = parent;

    return std::make_pair(std::move(server_session), std::move(client_session));
}

} // namespace Kernel
