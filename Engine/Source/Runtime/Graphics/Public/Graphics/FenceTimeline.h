#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/DeviceTypes.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

namespace rocks::graphics {

// backend가 제출 하나의 GPU 완료를 막고 기다리는 경계다. FenceTimeline의 관찰 스레드가 부른다.
// D3D12는 fence 이벤트, OpenGL은 공유 context의 glClientWaitSync로 구현한다.
class FenceWaitSource {
public:
    // 관찰 스레드가 시작한 직후·끝나기 직전에 그 스레드에서 한 번씩 부른다(OpenGL은 공유 context를
    // 붙이고 뗀다).
    virtual void OnWatcherStart() noexcept {
    }

    virtual void OnWatcherStop() noexcept {
    }

    // 제출 value(token은 Submitted에 넘긴 backend 값)가 GPU에서 끝날 때까지 기다린다. 끝났으면
    // true, 장치 손실·대기 실패면 false다. 반환 뒤 token은 다시 쓰지 않는다(ReleaseToken이 이어서
    // 불린다).
    virtual bool WaitSubmission(uint64 value, void* token) noexcept = 0;

    // token을 놓는다(OpenGL은 sync 객체를 지운다). 관찰 스레드나 Submitted를 부른 스레드(합칠 때)가
    // 부른다.
    virtual void ReleaseToken(void* token) noexcept {
        (void)token;
    }

protected:
    ~FenceWaitSource() = default;
};

// 한 장치·한 queue의 fence 진행을 다른 스레드에 공개하는 공용 구현이다(계획 11.2절 GpuFencePoint,
// RenderBase 4.4절 GX7). backend(D3D12·OpenGL·시험용 기록 장치)가 하나씩 소유하고 Device의 스레드
// 안전 fence API(LastSubmittedFence·IsFenceComplete·WaitFence·NotifyFence·IsLost)를 이것으로
// 구현한다.
//
// 사용 예(backend 구현):
//   FenceTimeline timeline(allocator, NewDeviceEpoch(), QueueKind::Graphics);
//   timeline.StartWatcher(source, "D3D12Fence");   // 관찰 스레드가 완료를 게시한다
//   const uint64 value = ++fenceValue;              // 소유 스레드: queue Signal 뒤
//   timeline.Submitted(value, nullptr);
//   ...
//   timeline.StopWatcher();                         // 모든 제출이 끝난 뒤(WaitIdle 뒤)
//
// 사용 예(다른 스레드의 대기):
//   auto waited = timeline.Wait(point, platform::InfiniteTimeout);
//   if (!waited) { /* Destroyed: 장치 손실 */ }
//
// 계약:
// - value는 1부터 제출 순서대로 커진다. 0은 "제출 없음"이다. 완료 값은 단조 증가한다.
// - Submitted는 소유 스레드 하나만 부른다. Publish·MarkLost는 어느 스레드에서나 부른다(관찰 스레드,
//   소유 스레드의 직접 관찰, 시험).
// - 장치 손실(MarkLost) 뒤 미완료 대기는 모두 Destroyed로 깨어나고 이후 대기·통지도 Destroyed다.
//   이미 완료된 지점은 손실 뒤에도 완료다.
// - 통지 함수는 잠금 밖에서 정확히 한 번 불린다: 완료면 None, 손실이면 Destroyed, 완료 전에
//   타임라인이 닫히면(Close) ShuttingDown이다. 이미 결정된 지점이면 Notify를 부른 스레드에서 바로
//   불린다.
// - 소멸 전에 StopWatcher와 Close를 부른다(소멸자가 대신하지만 관찰 스레드 Join이 끝나지 않을 수
//   있으므로 모든 제출이 끝난 뒤에 소멸시킨다).
class FenceTimeline {
public:
    FenceTimeline(Allocator& allocator, uint64 deviceEpoch, QueueKind queue) noexcept;
    ~FenceTimeline() noexcept;

    FenceTimeline(const FenceTimeline&) = delete;
    FenceTimeline& operator=(const FenceTimeline&) = delete;

    // 관찰 스레드를 시작한다. 실패: 이미 시작 Busy, 스레드 생성 실패 OutOfMemory. source는
    // StopWatcher까지 살아야 한다.
    Result<void> StartWatcher(FenceWaitSource& source, const char* threadName) noexcept;

    // 관찰 스레드를 끝낸다. 대기 중인 제출을 모두 관찰(완료 또는 손실)한 뒤 Join한다. 그래서 모든
    // 제출이 끝나거나 장치가 손실된 뒤에 부른다. 시작하지 않았으면 아무것도 하지 않는다.
    void StopWatcher() noexcept;

    // 소유 스레드가 queue에 value를 신호했음을 알린다. 관찰 스레드가 있으면 token과 함께 관찰
    // 목록에 넣는다. 목록이 가득 차면(MaxPendingSubmissions) 가장 최근 항목을 이 제출로 바꾼다(뒤
    // 제출의 완료가 앞 제출의 완료를 뜻한다). 바뀐 항목의 token은 이 스레드에서 ReleaseToken한다.
    void Submitted(uint64 value, void* token) noexcept;

    // value까지 끝났음을 게시한다(완료 값보다 작으면 무시). 대기자를 깨우고 만족한 통지를 부른다.
    void Publish(uint64 completed) noexcept;

    // 장치 손실을 게시한다. 미완료 대기·통지를 Destroyed로 끝낸다. 여러 번 불러도 된다.
    void MarkLost() noexcept;

    // 남은 통지를 ShuttingDown으로 끝내고 이후 Notify를 ShuttingDown으로 거부한다(장치 소멸).
    void Close() noexcept;

    // value의 완료 지점이다(value 0은 null 지점).
    GpuFencePoint Point(uint64 value) const noexcept;

    uint64 DeviceEpoch() const noexcept {
        return deviceEpoch_;
    }

    GpuFencePoint LastSubmitted() const noexcept;

    uint64 Completed() const noexcept;

    bool IsLost() const noexcept;

    // point가 끝났는지다. 실패: 다른 장치·queue의 지점 WrongContext, 아직 제출되지 않은 값
    // InvalidArgument, 미완료 상태의 장치 손실 Destroyed. null 지점은 끝난 것으로 본다.
    Result<bool> IsComplete(GpuFencePoint point) const noexcept;

    // point가 끝날 때까지 기다린다. timeoutMs가 지나면 TimedOut 값이다(취소하지 않는다). 실패는
    // IsComplete와 같다(손실은 기다리던 중에도 Destroyed로 깨운다).
    Result<platform::WaitResult> Wait(GpuFencePoint point,
                                      uint32 timeoutMs = platform::InfiniteTimeout) noexcept;

    // point가 결정되면 callback(result, context)을 부른다(위 계약). 실패: IsComplete의 인자 오류,
    // 닫힌 뒤 ShuttingDown, 등록 칸 할당 실패는 그 오류다(실패면 callback을 부르지 않는다).
    Result<void> Notify(GpuFencePoint point, FenceCallback callback, void* context) noexcept;

    // 아직 결정되지 않은 통지 수다(진단·시험).
    usize PendingNotifications() const noexcept;

private:
    struct Waiter {
        uint64 value = 0;
        FenceCallback callback = nullptr;
        void* context = nullptr;
    };

    struct PendingSubmission {
        uint64 value = 0;
        void* token = nullptr;
    };

    static constexpr usize MaxPendingSubmissions = 64;

    static void RunWatcher(void* raw) noexcept;
    Result<void> CheckPoint(GpuFencePoint point) const noexcept;
    // 결정된 통지 하나를 목록에서 빼서 돌려준다. 없으면 false다. gate_ 안에서 부른다.
    bool TakeDecidedLocked(Waiter& out, ErrorCode& result) noexcept;
    void FireDecided() noexcept;

    const uint64 deviceEpoch_;
    const QueueKind queue_;
    mutable platform::Mutex gate_;
    mutable platform::ConditionVariable changed_;
    // 관찰 스레드가 새 제출·종료 요청을 기다린다.
    platform::ConditionVariable watcherWake_;
    Array<Waiter> waiters_;
    PendingSubmission pending_[MaxPendingSubmissions];
    usize pendingHead_ = 0;
    usize pendingCount_ = 0;
    platform::Thread watcher_;
    FenceWaitSource* source_ = nullptr;
    uint64 submitted_ = 0;
    uint64 completed_ = 0;
    bool lost_ = false;
    bool closed_ = false;
    bool watcherRunning_ = false;
    bool watcherStopping_ = false;
};

// compute queue가 없는 장치(D3D11·OpenGL ES)의 Device::QueueWait다. graphics 지점끼리만 받고 할
// 일이 없다(같은 queue는 제출 순서). 실패: AsyncCompute 쪽 Unsupported, 그 밖은
// timeline.IsComplete의 인자 오류(WrongContext·InvalidArgument·Destroyed).
Result<void> SingleQueueWait(const FenceTimeline& timeline, QueueKind queue,
                             GpuFencePoint point) noexcept;

} // namespace rocks::graphics
