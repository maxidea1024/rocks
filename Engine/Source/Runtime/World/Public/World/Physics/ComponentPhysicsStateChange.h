#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

namespace rocks {

// 컴포넌트 물리 상태 변화다(PrimitiveComponent::componentPhysicsStateChanged — UE
// EComponentPhysicsStateChange, Docs/Design/PhysicsModule 7.1.19절).
// - Created: 물리 body를 만들었다(시스템 상태 생성·재생성).
// - Destroyed: 물리 body를 지웠다(시스템 상태 파괴·재생성).
ROCKS_ENUM()
enum class ComponentPhysicsStateChange : uint8 {
    Created,
    Destroyed
};

} // namespace rocks
