#include "ProgressInternal.h"

#include <Platform/Fatal.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressBindingSuspend.h>

namespace rocks {

namespace {

using detail::ProgressBindingFrame;
using detail::ProgressThreadState;

// 바인딩·중지 칸을 맨 위에서 뺀다. 생성의 역순·같은 스레드·그 위 구간 없음을 확인한다.
ProgressBindingFrame BindingPopFrame(uint32 depth, const detail::ProgressTaskRecord* record,
                                     const char* misuse) noexcept {
    ProgressThreadState& thread = detail::ProgressThread;
    if (thread.depth != depth || thread.frames[depth - 1].record != record) {
        platform::Fatal(misuse);
    }

    const ProgressBindingFrame frame = thread.frames[depth - 1];
    if (frame.scopeDepth != 0) {
        platform::Fatal("ProgressBinding or ProgressBindingSuspend ended while a ProgressScope "
                        "opened above it is still open");
    }

    thread.frames[depth - 1] = {};
    --thread.depth;
    return frame;
}

} // namespace

ProgressBinding::ProgressBinding(ProgressBinding&& other) noexcept
    : record_(other.record_), depth_(other.depth_) {
    other.record_ = nullptr;
    other.depth_ = 0;
}

ProgressBinding::~ProgressBinding() noexcept {
    if (depth_ == 0) {
        return;
    }

    const ProgressBindingFrame frame = BindingPopFrame(
        depth_, record_, "ProgressBinding destroyed out of order or on another thread");
    --detail::ProgressThread.bindingCount;
    if (frame.primary) {
        frame.record->primaryBound.Store<platform::MemoryOrder::Release>(0u);
    }

    detail::ReleaseRecord(frame.record);
}

Result<ProgressBinding> ProgressBinding::Bind(ProgressTaskId id,
                                              ProgressBindingRole role) noexcept {
    ProgressThreadState& thread = detail::ProgressThread;
    detail::ProgressHubState& hub = detail::ProgressHubGlobal;
    detail::ProgressTaskRecord* record = nullptr;
    {
        platform::LockGuard lock(hub.mutex);
        const auto phase =
            static_cast<detail::ProgressHubPhase>(hub.phase.Load<platform::MemoryOrder::Acquire>());
        if (phase == detail::ProgressHubPhase::Uninitialized) {
            return Result<ProgressBinding>::Failure(ErrorCode::NotReady);
        }

        if (phase == detail::ProgressHubPhase::ShutDown) {
            return Result<ProgressBinding>::Failure(ErrorCode::ShuttingDown);
        }

        for (uint32 index = 0; index < hub.liveCount; ++index) {
            if (hub.live[index]->id == id) {
                record = hub.live[index];
                break;
            }
        }

        if (record == nullptr) {
            const bool issued =
                id != NoProgressTask && id < hub.nextTaskId.Load<platform::MemoryOrder::Relaxed>();
            return Result<ProgressBinding>::Failure(issued ? ErrorCode::StaleHandle
                                                           : ErrorCode::NotFound);
        }

        if (thread.bindingCount >= MaxProgressBindingDepth ||
            thread.depth >= detail::MaxProgressBindingFrames) {
            return Result<ProgressBinding>::Failure(ErrorCode::CapacityExceeded);
        }

        if (role == ProgressBindingRole::Primary) {
            uint32 expected = 0;
            if (!record->primaryBound.CompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                            platform::MemoryOrder::Acquire>(
                    expected, 1u)) {
                return Result<ProgressBinding>::Failure(ErrorCode::AlreadyExists);
            }
        }

        detail::RetainRecord(record);
    }

    ProgressBindingFrame& frame = thread.frames[thread.depth];
    frame.record = record;
    frame.topScope = nullptr;
    frame.scopeDepth = 0;
    frame.primary = role == ProgressBindingRole::Primary;
    ++thread.depth;
    ++thread.bindingCount;
    return Result<ProgressBinding>::Success(ProgressBinding(thread.depth, record));
}

ProgressBindingSuspend::ProgressBindingSuspend() noexcept {
    ProgressThreadState& thread = detail::ProgressThread;
    if (thread.suspendCount >= MaxProgressBindingSuspendDepth ||
        thread.depth >= detail::MaxProgressBindingFrames) {
        platform::Fatal("ProgressBindingSuspend nested deeper than MaxProgressBindingSuspendDepth");
    }

    thread.frames[thread.depth] = {};
    ++thread.depth;
    ++thread.suspendCount;
    depth_ = thread.depth;
}

ProgressBindingSuspend::~ProgressBindingSuspend() noexcept {
    (void)BindingPopFrame(depth_, nullptr,
                          "ProgressBindingSuspend destroyed out of order or on another thread");
    --detail::ProgressThread.suspendCount;
}

} // namespace rocks
