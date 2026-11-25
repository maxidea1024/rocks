#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/SceneComponent.h>

namespace rocks {

// 표면에 투영하는 decal이다(UE UDecalComponent — Docs/Design/EngineRuntime 27.4절). decal
// 머티리얼은 에셋 참조라 World 위 모듈에서 넣는다. 데이터만 있다: 렌더 연결 전에는 그리지 않는다.
// 수명(fade 뒤 파괴)은 끝까지 동작한다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API DecalComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값이다(DecalComponent.cpp:233–241).
    static constexpr float DefaultFadeScreenSize = 0.01f;
    static constexpr double DefaultSizeX = 128.0;
    static constexpr double DefaultSizeYZ = 256.0;

    explicit DecalComponent(Allocator& allocator) noexcept;

    int32 SortOrder() const noexcept {
        return sortOrder_;
    }

    void SetSortOrder(int32 order) noexcept;

    float FadeScreenSize() const noexcept {
        return fadeScreenSize_;
    }

    // 오류: 비유한(아래 시간·크기 setter도 같다 — UE에 범위 제한이 없다).
    Result<void> SetFadeScreenSize(float size) noexcept;

    float FadeStartDelay() const noexcept {
        return fadeStartDelay_;
    }

    float FadeDuration() const noexcept {
        return fadeDuration_;
    }

    float FadeInStartDelay() const noexcept {
        return fadeInStartDelay_;
    }

    float FadeInDuration() const noexcept {
        return fadeInDuration_;
    }

    bool DestroysOwnerAfterFade() const noexcept {
        return destroyOwnerAfterFade_;
    }

    // 사라지기 시작할 때·걸리는 시간(초)을 정하고 수명을 그 합으로 둔다(UE SetFadeOut — :318). 합이
    // 넘쳐도 InvalidArgument다(바뀌지 않는다).
    Result<void> SetFadeOut(float startDelay, float duration,
                            bool destroyOwnerAfterFade = true) noexcept;

    // 나타나기 시작할 때·걸리는 시간(초)이다(UE SetFadeIn — :339).
    Result<void> SetFadeIn(float startDelay, float duration) noexcept;

    // 반 크기(cm, 지역 공간 — component scale 제외)다.
    const Vec3d& DecalSize() const noexcept {
        return decalSize_;
    }

    Result<void> SetDecalSize(const Vec3d& size) noexcept;

    const LinearColor& DecalColor() const noexcept {
        return decalColor_;
    }

    // 오류: 비유한 성분.
    Result<void> SetDecalColor(const LinearColor& color) noexcept;

    // 수명(초)을 정한다(UE SetLifeSpan — :270): 0보다 크면 그 시간 뒤 컴포넌트를 파괴하고,
    // destroyOwnerAfterFade이고 fade 시간이 있으면 소유 Actor도 파괴한다. 0 이하는 취소다. 오류:
    // 비유한.
    Result<void> SetLifeSpan(float seconds) noexcept;

    // 남은 수명(초, 없으면 0)이다.
    double RemainingLifeSpan() const noexcept {
        return remainingLife_;
    }

protected:
    // UE BeginPlay(:507): SetLifeSpan(fadeStartDelay + fadeDuration).
    void OnStart() noexcept override;
    // 남은 수명을 센다(World 타이머 대신 — 27.4절 ERD52).
    void OnTick(const TickContext& context) noexcept override;
    // 지역 (0, decalSize, |decalSize|)다(UE CalcBounds — :472).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sortOrder") int32 sortOrder_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "fadeScreenSize") float fadeScreenSize_ = DefaultFadeScreenSize;
    ROCKS_PROPERTY(ReadOnly, Name = "fadeStartDelay") float fadeStartDelay_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "fadeDuration") float fadeDuration_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "fadeInStartDelay") float fadeInStartDelay_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "fadeInDuration") float fadeInDuration_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "destroyOwnerAfterFade") bool destroyOwnerAfterFade_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "decalSize")
    Vec3d decalSize_ = Vec3d(DefaultSizeX, DefaultSizeYZ, DefaultSizeYZ);
    ROCKS_PROPERTY(ReadOnly, Name = "decalColor") LinearColor decalColor_ = LinearColor::White();
    ROCKS_NATIVE() double remainingLife_ = 0.0;
};

} // namespace rocks
