#pragma once

#include <Core/Text/Name.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAssets/ColorCurveValue.h>
#include <Meta/Annotations.h>
#include <Object/SoftObjectRef.h>

namespace rocks {

// 타임라인 색 트랙이다(UE FTimelineLinearColorTrack — Docs/Design/EngineRuntime 32.2절, ERD71).
// 커브 원천 규칙은 TimelineFloatTrack과 같다(로드 사본은 ApplyLoadedColorCurve). 평가는 늘 조정
// 적용 식이다(UE GetLinearColorValue — 인라인은 그 값의 조정 필드, 기본 항등 조정이어도 HSV 왕복과
// LDR 명도 자르기를 거친다). adjusted 플래그는 보지 않는다.
ROCKS_STRUCT()
struct ROCKS_WORLDINTEGRATION_API TimelineColorTrack {
    ROCKS_GENERATED_BODY()

    explicit TimelineColorTrack(Allocator& allocator) noexcept
        : curve(allocator), curveAsset(allocator) {
    }

    // other를 깊게 복사한다(실패하면 그대로). 오류: 할당 실패.
    Result<void> TryCopyFrom(const TimelineColorTrack& other) noexcept;

    // 트랙 이름이다(colorTrackUpdated의 첫 인자).
    ROCKS_PROPERTY() Name trackName;
    // 인라인 커브다.
    ROCKS_PROPERTY() ColorCurveValue curve;
    // 커브 에셋이다(null이면 인라인).
    ROCKS_PROPERTY() SoftObjectRef<ColorCurveAsset> curveAsset;
};

} // namespace rocks
