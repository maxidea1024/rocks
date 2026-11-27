#pragma once

#include <Core/CancelToken.h>
#include <Core/Memory/Allocator.h>
#include <Engine/PendingWorkSource.h>
#include <Localization/Text.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>

namespace rocks {

// 수와 일 구간으로 추적하는 대기 작업 종류다(Docs/Design/ProgressReporting 8장 PG5 연결). 일을 맡은
// 쪽(예: Development 셰이더 공급자)이 일 구간을 열고 더할 일·끝난 일 수를 넣으면, 등록소의
// 남은 수·FinishAll·Shutdown 계약을 이 객체가 채운다.
//
// - RemainingCount = max(더한 수 - 끝난 수, 일 중이면 1)이다(UE FShaderCompilingManager::
//   GetNumRemainingAssets처럼 일 중에는 0을 돌려주지 않는다). 원자 읽기뿐이다.
// - FinishAll은 일 구간이 모두 닫힐 때까지 막는다. 등록소가 바인딩한 하위 작업에 끝난 수를 계수로
//   보고한다(끝난 수가 오를 때와 일 구간이 닫힐 때 깬다 — 시간 polling 없음).
// - FinishFor·CancelFor·ApplyCompleted는 아무 일도 하지 않는다(객체 대상이 없고, 결과 적용은 일을
//   맡은 쪽이 자기 자리에서 한다).
// - Shutdown은 Token을 취소하고(되돌리지 않는다) 일 구간이 모두 닫힐 때까지 기다린다. 두 번째부터는
//   기다리기만 한다.
//
// 사용 예:
//   CountedPendingWork shaders;
//   (void)shaders.Initialize(allocator, "Shaders", shadersText, runBefore);
//   (void)registry.Register(shaders);
//   {
//       CountedPendingWork::WorkScope work = shaders.BeginWork();   // 일 구간
//       shaders.AddWork(jobs);                    // 아무 스레드
//       ... shaders.Token() ...                   // 취소
//       shaders.FinishWork(1);                    // 아무 스레드
//   }
//
// 스레드: AddWork·FinishWork·BeginWork·WorkScope·Token·RemainingCount는 아무 스레드에서나 부른다.
// Initialize와 PendingWorkSource의 나머지 함수는 등록소 소유 스레드다. 일 구간이 살아 있는 채
// 소멸하면 안 된다.
class ROCKS_ENGINE_API CountedPendingWork final : public PendingWorkSource {
public:
    // 일 구간이다(이동만). 살아 있는 동안 "일 중"이고, 소멸이 구간을 닫고 기다리는 쪽을 깨운다.
    class ROCKS_ENGINE_API WorkScope {
    public:
        WorkScope() noexcept = default;
        WorkScope(WorkScope&& other) noexcept;
        WorkScope& operator=(WorkScope&& other) noexcept;
        WorkScope(const WorkScope&) = delete;
        WorkScope& operator=(const WorkScope&) = delete;
        ~WorkScope() noexcept;

    private:
        friend class CountedPendingWork;

        explicit WorkScope(CountedPendingWork* owner) noexcept : owner_(owner) {
        }

        void Release() noexcept;

        CountedPendingWork* owner_ = nullptr;
    };

    CountedPendingWork() noexcept;
    ~CountedPendingWork() noexcept override;
    CountedPendingWork(const CountedPendingWork&) = delete;
    CountedPendingWork& operator=(const CountedPendingWork&) = delete;

    // 종류 이름·먼저 끝낼 종류(둘 다 정적 수명 — 이 객체보다 오래 산다)와 표시 문구(Clone)를 정하고
    // 취소 source를 만든다. 오류: 빈 이름 InvalidArgument, 이미 정했으면 AlreadyExists, 문구
    // Clone·취소 source 할당 실패는 그 오류.
    Result<void> Initialize(Allocator& allocator, StringView name, const Text& displayName,
                            Span<const StringView> runBefore = {}) noexcept;

    // 더할 일 수·끝난 일 수다(원자 더하기 — 잠금 없음).
    void AddWork(uint64 count) noexcept;
    void FinishWork(uint64 count) noexcept;

    // 일 구간을 연다(Shutdown 뒤에도 열린다 — 그 일은 Token으로 바로 취소를 본다).
    WorkScope BeginWork() noexcept;

    // 이 종류의 취소 token이다(Shutdown이 취소한다). Initialize 전이면 취소될 수 없는 token이다.
    CancelToken Token() const noexcept;

    // 열린 일 구간 수다(시험·진단).
    uint32 ActiveWork() const noexcept;

    StringView Name() const noexcept override;
    const Text& DisplayName() const noexcept override;
    Span<const StringView> RunBefore() const noexcept override;
    uint64 RemainingCount() const noexcept override;
    Result<void> FinishFor(Span<Object* const> targets) noexcept override;
    void CancelFor(Span<Object* const> targets) noexcept override;
    Result<void> FinishAll() noexcept override;
    void Shutdown() noexcept override;
    void ApplyCompleted(const PendingWorkPumpParams& params) noexcept override;

private:
    void EndWork() noexcept;

    StringView name_;
    Span<const StringView> runBefore_;
    Text displayName_;
    CancelSource cancel_;
    platform::Atomic<uint64> added_{0};
    platform::Atomic<uint64> finished_{0};
    // 열린 일 구간 수(gate_ 아래에서 바꾸고 RemainingCount가 잠금 없이 읽는다)와 FinishAll 대기
    // 수다.
    platform::Atomic<uint32> active_{0};
    platform::Atomic<uint32> waiters_{0};
    platform::Mutex gate_;
    platform::ConditionVariable changed_;
    bool initialized_ = false;
};

} // namespace rocks
