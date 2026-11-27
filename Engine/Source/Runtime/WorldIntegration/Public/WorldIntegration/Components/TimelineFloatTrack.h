#pragma once

#include <Core/Text/Name.h>
#include <CurveAssets/FloatCurveAsset.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>
#include <Object/SoftObjectRef.h>

namespace rocks {

// 타임라인 float 트랙이다(UE FTimelineFloatTrack — Docs/Design/EngineRuntime 32.2절, ERD71).
// curveAsset이 null이면 인라인 curve가 트랙 커브다(Blueprint 안 커브 대응). 있으면 로드 시스템이
// 넣는 로드 사본(TimelineComponent::ApplyLoadedFloatCurve)이 트랙 커브이고, 사본 전에는 평가하지
// 않는다.
//
// 사용 예:
//   TimelineFloatTrack track(allocator);
//   track.trackName = fade;
//   (void)track.curve.TryAddKey(0.0f, 0.0f);
ROCKS_STRUCT()
struct ROCKS_WORLDINTEGRATION_API TimelineFloatTrack {
    ROCKS_GENERATED_BODY()

    explicit TimelineFloatTrack(Allocator& allocator) noexcept
        : curve(allocator), curveAsset(allocator) {
    }

    // other를 깊게 복사한다(실패하면 그대로). 오류: 할당 실패.
    Result<void> TryCopyFrom(const TimelineFloatTrack& other) noexcept;

    // 트랙 이름이다(floatTrackUpdated의 첫 인자, UE TrackName).
    ROCKS_PROPERTY() Name trackName;
    // 인라인 커브다.
    ROCKS_PROPERTY() TangentCurve curve;
    // 커브 에셋이다(null이면 인라인).
    ROCKS_PROPERTY() SoftObjectRef<FloatCurveAsset> curveAsset;
};

} // namespace rocks
