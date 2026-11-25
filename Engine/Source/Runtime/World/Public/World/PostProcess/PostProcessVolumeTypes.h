#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

namespace rocks {

// 경계 상자 밖이라 거리를 재지 않았다는 표지 거리다(UE FLT_MAX — PostProcessComponent.cpp:187).
inline constexpr double PostProcessOutsideDistance = NumericLimits<float>::Max();

// 후처리 볼륨 영역 판정 결과다(UE EncompassesPoint의 반환값과 OutDistanceToPoint — Docs/Design/
// EngineRuntime 28.4절). distance는 형상 표면까지(cm, 안이면 0)다.
struct PostProcessEncompass {
    bool inside = false;
    double distance = 0.0;
};

// 정렬·가중에 쓰는 볼륨 속성이다(UE FPostProcessVolumeProperties — PostProcessComponent.h:65).
// size는 경계 상자 부피(cm³, 경계가 없으면 PostProcessUnboundSize)다.
struct PostProcessVolumeProperties {
    bool enabled = false;
    bool unbound = true;
    float blendRadius = 0.0f;
    float blendWeight = 0.0f;
    float priority = 0.0f;
    double size = 0.0;
};

// 경계 없는 볼륨의 크기다(UE DBL_MAX).
inline constexpr double PostProcessUnboundSize = NumericLimits<double>::Max();

} // namespace rocks
