#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Engine/PendingWorkSource.h>
#include <Platform/Sync.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

// 대기 작업 등록소다(Docs/Design/ProgressReporting 8장, UE FAssetCompilingManager 대응). 비동기
// 빌드 종류(PendingWorkSource)를 모아 남은 수를 합치고, 저장·쿠킹·Tryout 전에 전부(FinishAll) 또는
// 대상만 (FinishFor) 끝내며, 소유 스레드 프레임마다 끝난 결과를 적용한다(ApplyCompleted). Engine이
// 하나를 갖는다(Engine::PendingWork — 매 프레임 ApplyCompleted, Shutdown 전에 Shutdown). 엔진 없이
// 도는 명령은 자기 주 스레드에서 만들어 자기 반복에서 ApplyCompleted를 부른다.
//
// 사용 예(모듈 Startup·Shutdown):
//   (void)engine.PendingWork().Register(shaderWork_);
//   ...
//   shaderWork_.Shutdown();
//   (void)engine.PendingWork().Unregister(shaderWork_);
//
// 규칙:
// - 소유 스레드(만든 스레드, AdoptCurrentThread로 바꿈)가 아니면 RemainingCount 말고는
//   WrongThread다(Shutdown은 Error 로그만 남기고 아무 일도 하지 않는다).
// - RunBefore 순서로 끝낸다. 순환이 생기면 등록은 받고 앞서 등록한 source가 새 source를 가리키는
//   의존을 무시하며 Error 로그를 남긴다(UE와 같다). 없는 이름은 무시한다.
// - 등록소 호출(FinishAll·FinishFor·CancelFor·ApplyCompleted·Shutdown) 도중 source 안에서 부른
//   Register·Unregister·호출은 Busy다.
// - FinishAll은 모든 source를 부른다. 할 일이 있으면 Blocking 진행 작업 "Finishing background
//   work"(ProgressHub가 켜져 있을 때)를 열고 종류별 하위 작업을 이 스레드에 바인딩한다(source가 그
//   위에서 ProgressScope로 보고).
class ROCKS_ENGINE_API PendingWorkRegistry {
public:
    // allocator는 등록 목록·진행 문구를 할당한다. 만든 스레드가 소유 스레드다.
    explicit PendingWorkRegistry(Allocator& allocator) noexcept;
    // 등록이 남아 있으면 Warning이다(모듈이 Unregister하지 않았다).
    ~PendingWorkRegistry() noexcept;
    PendingWorkRegistry(const PendingWorkRegistry&) = delete;
    PendingWorkRegistry& operator=(const PendingWorkRegistry&) = delete;

    // 소유 스레드를 이 스레드로 정한다(Engine::Initialize가 부른다). 등록이 있으면 Busy다.
    Result<void> AdoptCurrentThread() noexcept;

    // 오류: 빈 이름 InvalidArgument, 같은 이름·같은 source AlreadyExists, Shutdown 뒤 ShuttingDown,
    // 등록소 호출 도중 Busy, 소유 스레드 밖 WrongThread, 할당 실패 OutOfMemory(등록하지 않은 상태로
    // 되돌린다).
    Result<void> Register(PendingWorkSource& source) noexcept;

    // 할당하지 않는다(순서는 그대로 두고 그 칸만 뺀다). 오류: 등록되지 않았으면 NotFound, 등록소
    // 호출 도중 Busy, 소유 스레드 밖 WrongThread.
    Result<void> Unregister(PendingWorkSource& source) noexcept;

    // 모든 source의 남은 수 합이다. 아무 스레드에서나 부를 수 있다. 목록 잠금 안에서 각 source의
    // RemainingCount를 부른다(PendingWorkSource 계약).
    uint64 RemainingCount() const noexcept;

    // 모든 source를 RunBefore 순서로 끝낸다(위 규칙). 오류: 소유 스레드 밖 WrongThread, 도중
    // 재진입 Busy, source 오류는 처음 것(나머지 source는 계속 끝낸다).
    Result<void> FinishAll() noexcept;

    // 모든 source에 같은 targets를 넘긴다. 빈 목록이면 아무 일도 하지 않는다. 오류는 FinishAll과
    // 같다(CancelFor는 source 오류가 없다).
    Result<void> FinishFor(Span<Object* const> targets) noexcept;
    Result<void> CancelFor(Span<Object* const> targets) noexcept;

    // 모든 source에 같은 params로 끝난 결과를 적용하게 한다. 오류: WrongThread, 도중 재진입 Busy.
    Result<void> ApplyCompleted(const PendingWorkPumpParams& params) noexcept;

    // 모든 source의 Shutdown을 RunBefore 순서(UE와 같다)로 부른다. 그 뒤 Register는
    // ShuttingDown이고 Unregister는 된다. 두 번째부터는 아무 일도 하지 않는다. 소유 스레드 밖이거나
    // 등록소 호출 도중이면 Error 로그를 남기고 아무 일도 하지 않는다.
    void Shutdown() noexcept;

    // RunBefore 순서의 source 수와 index번째 이름이다(진단·시험). 소유 스레드 전용이다(밖에서는 0과
    // 빈 이름).
    usize SourceCount() const noexcept;
    StringView SourceNameAt(usize orderIndex) const noexcept;

private:
    struct Entry {
        PendingWorkSource* source = nullptr;
        // 등록 때 순환을 만들어, 다른 source의 RunBefore가 이 source를 가리키는 것을 무시한다(UE
        // AssetCompilingManagersWithValidDependencies에서 뺀 manager).
        bool excludedAsDependency = false;
    };

    enum class OrderResult : uint8 {
        Sorted,
        Cycle,
        OutOfMemory,
    };

    bool IsOwnerThread() const noexcept;
    // RunBefore가 가리킬 수 있는 source 중 이름이 name인 것의 색인이다.
    usize FindDependency(StringView name) const noexcept;
    // RunBefore 순서를 다시 계산한다. Sorted가 아니면 order_는 그대로다.
    OrderResult RebuildOrder() noexcept;
    // source 하나를 끝낸다(parent가 있으면 그 아래 하위 작업을 바인딩해서).
    ErrorCode FinishSource(PendingWorkSource& source, ProgressTaskId parent) noexcept;

    Allocator& allocator_;
    // 목록은 소유 스레드가 바꾸고 RemainingCount가 다른 스레드에서 읽는다.
    mutable platform::Mutex mutex_;
    Array<Entry> entries_;
    // RunBefore 순서의 entries_ 색인이다.
    Array<usize> order_;
    uint64 ownerThread_ = 0;
    bool inCall_ = false;
    bool shutDown_ = false;
};

} // namespace rocks
