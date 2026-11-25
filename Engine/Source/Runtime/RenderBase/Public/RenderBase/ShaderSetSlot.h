#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Sync.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/ShaderSet.h>

// 셰이더 집합 교체 자리다(Docs/Design/RenderBase 9.8절 "구현 설계", RB4). 한 종류(엔진·묶음)의
// 현재 셰이더 집합과 교체 후보를 가진다. 셰이더 시스템 SH4(감시 재컴파일)가 새 집합을 Propose하고,
// 렌더 lane이 프레임 경계에서 ApplyPending으로 바꾼다. 옛 집합은 Retire하고(새 lease·새 장치 셰이더
// 생성 거부, 파이프라인 캐시 항목 회수), 진행 중 프레임이 잡은 lease가 모두 풀린 뒤
// CollectRetired가 파괴한다. 검사에 실패한 후보는 버리고 현재 집합을 유지한다(계획 14.1절 "hot
// reload 실패 시 이전 검증 버전 유지"). UE는 재컴파일 뒤 새 셰이더 맵을 끼우고 옛 맵은 참조가
// 사라질 때 지운다. Rocks는 그 시점을 lease와 프레임 경계로 명시한다.
//
// 사용 예(렌더 lane 프레임 작업 맨 앞):
//   auto swapped = engineShaders.ApplyPending(&pipelineCache);   // 후보가 Ready면 교체
//   (void)engineShaders.CollectRetired();                         // lease가 풀린 옛 집합 파괴
//   ShaderSet* set = engineShaders.Current();
//
// 사용 예(감시 재컴파일 — 어느 스레드에서나):
//   auto candidate = UniquePtr<ShaderSet>::TryMake(allocator, desc);
//   (void)candidate.Value()->OpenFromProvider(&provider);
//   (void)engineShaders.Propose(Move(candidate.Value()));
//
// 스레드: Propose는 어느 스레드에서나 부른다. Install·ApplyPending·CollectRetired·Shutdown은 렌더
// lane(장치 소유 스레드 — ShaderSet::Retire·PipelineCache::RetireSet 규칙)에서 부른다. 조회는 어느
// 스레드에서나 부를 수 있다(Current가 돌려준 집합은 다음 ApplyPending까지 유효하다 — 넘어서 쓰려면
// lease를 잡는다).
namespace rocks::render {

class ROCKS_RENDERBASE_API ShaderSetSlot final {
public:
    explicit ShaderSetSlot(Allocator& allocator) noexcept;
    // Shutdown하지 않고 파괴하면 남은 집합을 Retire하지 못한다(장치를 쓰는 스레드를 모른다). 남은
    // 집합이 있으면 Fatal 대신 assert로 잡는다.
    ~ShaderSetSlot() noexcept;
    ShaderSetSlot(const ShaderSetSlot&) = delete;
    ShaderSetSlot& operator=(const ShaderSetSlot&) = delete;

    // 첫 집합을 둔다. 실패: 이미 현재 집합이 있음 Busy, null InvalidArgument, Ready가 아님
    // DependencyFailed(집합은 버린다).
    Result<void> Install(UniquePtr<ShaderSet>&& set) noexcept;

    // 교체 후보를 받는다. 이미 후보가 있으면 옛 후보를 버린다(마지막 후보가 이긴다). 실패: null
    // InvalidArgument, Failed·Closed·Retired인 후보 DependencyFailed(거부 수를 올리고 진단을
    // 남긴다 — 현재 집합은 그대로다).
    Result<void> Propose(UniquePtr<ShaderSet>&& candidate) noexcept;

    // 프레임 경계에서 후보를 적용한다. 후보가 없거나 공급 대기면 false(다음 경계로 미룬다), Ready면
    // 교체하고 true다: 옛 집합의 cache 항목을 RetireSet하고(cache가 null이 아니면) 옛 집합을
    // Retire해 회수 대기 목록에 둔다. 후보가 Failed가 되었으면 버리고 거부 수를 올린다(false).
    // 현재 집합이 없으면 Ready 후보를 그대로 현재로 둔다.
    bool ApplyPending(PipelineCache* cache) noexcept;

    // 회수 대기 목록에서 lease가 0인 집합을 파괴한다. 파괴한 수를 돌려준다.
    usize CollectRetired() noexcept;

    // 종료 4단계(5.5절): 후보를 버리고 현재 집합을 cache에서 회수·Retire한 뒤 CollectRetired한다.
    // lease가 남은 집합이 있으면 Busy다(closure 실패 — 강제로 파괴하지 않는다, 다시 부를 수 있다).
    Result<void> Shutdown(PipelineCache* cache) noexcept;

    // 현재 집합이다(없으면 null).
    ShaderSet* Current() const noexcept;

    // 통계다: 교체 수, 거부(Propose 거부·적용 때 실패) 수, 회수 대기 중인 옛 집합 수.
    uint64 Swaps() const noexcept;
    uint64 Rejections() const noexcept;
    usize RetiredCount() const noexcept;

    // 마지막 거부의 영어 이유다(집합의 ProblemDetail 앞부분, 다음 거부까지 유효하다).
    StringView LastRejection() const noexcept;

private:
    void RejectLocked(const ShaderSet& set, const char* reason) noexcept;

    mutable platform::Mutex gate_;
    UniquePtr<ShaderSet> current_;
    UniquePtr<ShaderSet> pending_;
    Array<UniquePtr<ShaderSet>> retired_;
    uint64 swaps_ = 0;
    uint64 rejections_ = 0;
    char rejection_[256] = {};
};

} // namespace rocks::render
