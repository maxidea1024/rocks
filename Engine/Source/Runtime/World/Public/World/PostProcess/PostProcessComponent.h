#pragma once

#include <Meta/Annotations.h>
#include <World/PostProcess/PostProcessSettings.h>
#include <World/SceneComponent.h>

namespace rocks {

// 후처리 볼륨 컴포넌트다(UE UPostProcessComponent — Docs/Design/EngineRuntime 27.5절). 기본은 경계
// 없음(unbound)이다. 데이터와 덮어쓰기 계산(PostProcessBlending.h)만 있다: 카메라 위치로 볼륨을
// 모으는 일은 렌더(RS6)와 함께다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API PostProcessComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 경계 혼합 거리(cm, UE PostProcessComponent.cpp:20)다.
    static constexpr float DefaultBlendRadius = 100.0f;

    explicit PostProcessComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    const PostProcessSettings& Settings() const noexcept {
        return settings_;
    }

    // 오류: 범위 밖(IsValidPostProcessSettings 거짓 — InvalidArgument, 바뀌지 않는다). Mobility는
    // 보지 않는다(UE와 같다).
    Result<void> SetSettings(const PostProcessSettings& settings) noexcept;

    float Priority() const noexcept {
        return priority_;
    }

    // 오류: 비유한.
    Result<void> SetPriority(float priority) noexcept;

    float BlendRadius() const noexcept {
        return blendRadius_;
    }

    // 오류: 음수·비유한.
    Result<void> SetBlendRadius(float radius) noexcept;

    float BlendWeight() const noexcept {
        return blendWeight_;
    }

    // 오류: 비유한(UE는 UIMin·UIMax [0, 1]뿐이다 — 혼합이 1 넘는 값을 1로 자른다).
    Result<void> SetBlendWeight(float weight) noexcept;

    // 이 볼륨이 혼합에 들어가는지다(UE bEnabled). Component 활성(IsEnabled)과 다르다.
    bool IsPostProcessEnabled() const noexcept {
        return postProcessEnabled_;
    }

    void SetPostProcessEnabled(bool enabled) noexcept {
        postProcessEnabled_ = enabled;
    }

    // 참이면 경계 없이 어디서나 적용된다(UE bUnbound).
    bool IsUnbound() const noexcept {
        return unbound_;
    }

    void SetUnbound(bool unbound) noexcept {
        unbound_ = unbound;
    }

private:
    ROCKS_PROPERTY(ReadOnly, Name = "settings") PostProcessSettings settings_;
    ROCKS_PROPERTY(ReadOnly, Name = "priority") float priority_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "blendRadius") float blendRadius_ = DefaultBlendRadius;
    ROCKS_PROPERTY(ReadOnly, Name = "blendWeight") float blendWeight_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "postProcessEnabled") bool postProcessEnabled_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "unbound") bool unbound_ = true;
};

} // namespace rocks
