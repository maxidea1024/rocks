#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 모디파이어를 적용한 문맥이다. deltaSeconds는 dilation을 뺀 dt(4.5절). hasPlayer는 평가기가
// 플레이어(UE PlayerController)에 붙어 있는지, fov는 카메라 FOV(도 — 0 이하면 카메라 없음)다.
// FovScaling은 플레이어가 없으면 값을 바꾸지 않고, 카메라가 없으면 fovScale만 곱한다(UE와 같다).
struct ModifierContext {
    double deltaSeconds = 0.0;
    float fov = 0.0f;
    bool hasPlayer = false;
};

} // namespace rocks::input
