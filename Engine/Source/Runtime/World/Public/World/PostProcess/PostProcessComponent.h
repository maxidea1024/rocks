#pragma once

#include <Meta/Annotations.h>
#include <World/PostProcess/PostProcessSettings.h>
#include <World/PostProcess/PostProcessVolumeTypes.h>
#include <World/SceneComponent.h>

namespace rocks {

// 후처리 볼륨 컴포넌트다(UE UPostProcessComponent — Docs/Design/EngineRuntime 27.5·28.4절). 기본은
// 경계 없음(unbound)이다. 경계가 있으면 부착 부모 PrimitiveComponent의 bounds·형상이
// 영역이다(부모가 Shape면 정확한 형상 거리). 볼륨 모으기·혼합은 PostProcessVolumeBlending.h다. view
// 위치 공급(카메라)은 렌더(RS6)와 함께다.
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

    // 영역 경계다(UE GetPostProcessBounds): unbound가 아니고 부착 부모가 PrimitiveComponent면 부모
    // Bounds()를 bounds에 쓰고 참이다. 그 밖에는 거짓이다(경계 없음 — 어디서나 적용).
    bool TryPostProcessBounds(ComponentBounds& bounds) const noexcept;

    // 점이 영역 안인지와 형상까지 거리다(UE EncompassesPoint — 28.4절 순서):
    // - 경계가 없으면 {참, 0}.
    // - 경계 상자를 blendRadius + sphereRadius만큼 넓혀 밖이면 {거짓, PostProcessOutsideDistance}.
    // - 부모가 Shape면 형상 거리 d로 {d ≤ sphereRadius, d}(형상 거리를 못 재면 밖 —
    //   {거짓, PostProcessOutsideDistance}).
    // - 그 밖의 primitive면 {참, 0}(UE 마지막 분기).
    PostProcessEncompass EncompassesPoint(const Vec3d& point,
                                          float sphereRadius = 0.0f) const noexcept;

    // 정렬·가중 속성이다(UE GetProperties): enabled = postProcessEnabled이고 보인다(IsVisible),
    // unbound = unbound이거나 경계 없음, size = 경계 상자 부피(경계 없으면 PostProcessUnboundSize).
    PostProcessVolumeProperties VolumeProperties() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "settings") PostProcessSettings settings_;
    ROCKS_PROPERTY(ReadOnly, Name = "priority") float priority_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "blendRadius") float blendRadius_ = DefaultBlendRadius;
    ROCKS_PROPERTY(ReadOnly, Name = "blendWeight") float blendWeight_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "postProcessEnabled") bool postProcessEnabled_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "unbound") bool unbound_ = true;
};

} // namespace rocks
