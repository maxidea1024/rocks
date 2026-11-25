#pragma once

#include <World/PostProcess/PostProcessSettings.h>

namespace rocks {

// source의 override가 켜진 값으로 destination을 weight만큼 덮는다(UE OverridePostProcessSettings —
// SceneView.cpp:1622, Docs/Design/EngineRuntime 27.5절): 연속 값은 Lerp(dest, src, w), 이산 값
// (enum·bool·int)은 w > 0이면 그대로다. 초점 거리는 어느 쪽이 0("DOF 끔")이면 그대로 쓴다. weight
// > 1(+inf 포함)은 1로 자르고, 0 이하·NaN이면 아무것도 바꾸지 않는다. destination의 override
// 플래그는 바꾸지 않는다(UE와 같다 — 결과 설정은 값만 쓴다).
//
// 사용 예(낮은 우선순위부터 차례로 덮는다):
//   PostProcessSettings result;
//   OverridePostProcessSettings(result, volumeA.Settings(), volumeA.BlendWeight());
ROCKS_WORLD_API void OverridePostProcessSettings(PostProcessSettings& destination,
                                                 const PostProcessSettings& source,
                                                 float weight) noexcept;

} // namespace rocks
