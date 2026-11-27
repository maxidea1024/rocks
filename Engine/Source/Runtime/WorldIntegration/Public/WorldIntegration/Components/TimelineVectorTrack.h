#pragma once

#include <Core/Text/Name.h>
#include <CurveAssets/VectorCurveAsset.h>
#include <CurveAssets/VectorCurveValue.h>
#include <Meta/Annotations.h>
#include <Object/SoftObjectRef.h>

namespace rocks {

// 타임라인 벡터 트랙이다(UE FTimelineVectorTrack — Docs/Design/EngineRuntime 32.2절, ERD71). 커브
// 원천 규칙은 TimelineFloatTrack과 같다(로드 사본은 ApplyLoadedVectorCurve).
ROCKS_STRUCT()
struct ROCKS_WORLDINTEGRATION_API TimelineVectorTrack {
    ROCKS_GENERATED_BODY()

    explicit TimelineVectorTrack(Allocator& allocator) noexcept
        : curve(allocator), curveAsset(allocator) {
    }

    // other를 깊게 복사한다(실패하면 그대로). 오류: 할당 실패.
    Result<void> TryCopyFrom(const TimelineVectorTrack& other) noexcept;

    // 트랙 이름이다(vectorTrackUpdated의 첫 인자).
    ROCKS_PROPERTY() Name trackName;
    // 인라인 커브다(성분마다 TangentCurve — fallback 0).
    ROCKS_PROPERTY() VectorCurveValue curve;
    // 커브 에셋이다(null이면 인라인).
    ROCKS_PROPERTY() SoftObjectRef<VectorCurveAsset> curveAsset;
};

} // namespace rocks
