#pragma once

#include <Animation/AnimationClip.h>
#include <AssetCooker/CookProcessor.h>

// 애니메이션 클립 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 7.6.1절 3 — MA6b1). AnimationClip의
// 쿠킹 패키지를 만든다: 클립 검증 → 스켈레톤 참조 풀기 → 원본 로드 → BuildClipPlayback(프로파일
// 목표 비율·클립 보간) → playback 채우기·쿠킹 패키지 쓰기 → 에디터 객체 되돌리기(성공·실패 모두).
// 너겟은 쓰지 않는다(v1 — 압축이 생기면 더한다). 스켈레톤은 Build 의존이라 바뀌면 쿠커가 다시
// 쿠킹한다(AnimationTypeHooks).
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   AnimationClipCookProcessor clipCook;
//   context.cookProcessors = {&RegisterAnimationClipCookProcessors, &clipCook};
namespace rocks {

// 처리기 정체성이다(출력 배치 — playback 채우기 — 가 바뀌면 버전 Guid를 바꾼다).
inline constexpr NuggetProcessorId AnimationClipCookProcessorId{
    Guid{0x524f434b414e4d43ull, 0x0000000000000001ull},
    Guid{0x524f434b414e4d56ull, 0x0000000000000001ull}};

// 애니메이션 클립 쿠킹 처리기다(TypeIdOf<AnimationClip>()에 등록). 한 스레드에서 쓴다(쿠킹 실행기와
// 같다). 경고: 원본 트랙 본이 스켈레톤에 없음 UnknownBoneTrack(setting = 원본 트랙), 쿠킹에 넣지
// 않은 사용 중 변환 커브 UnsupportedSetting(setting = 그 수).
//
// Cook 오류: AnimationClip이 아님 TypeMismatch, 클립 검증 위반 그 진단(InvalidClipData — detail =
// index), 스켈레톤이 비었거나 풀리지 않음 MissingDependency(InvalidArgument — 풀기 오류는 그
// 오류), 원본 로드 실패 그 진단(CorruptPackage 등 — detail = 트랙), 쿠킹 변환 실패 그
// 진단(InvalidClipData), Registry Name pool·bulk 원본 없음 InvalidArgument(진단 None), 쓰기 오류,
// 할당 실패 OutOfMemory.
class ROCKS_ANIMATIONTOOLS_API AnimationClipCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return AnimationClipCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override;

    // 쿠킹 변환을 한 횟수다(시험).
    uint64 Builds() const noexcept {
        return builds_;
    }

private:
    mutable uint64 builds_ = 0;
};

// 쿠킹 처리기 등록이다(AssetCookProcessorSetup — state = AnimationClipCookProcessor*, 명령 동안
// 산다). 오류: null state InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_ANIMATIONTOOLS_API Result<void>
RegisterAnimationClipCookProcessors(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks
