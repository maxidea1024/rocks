#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// B4c 사용자 리플렉션 struct가 Core 값 타입 멤버를 담는 경우다(멤버 struct가 다른 모듈의 Binds
// 거울). 나머지 거울 타입을 모두 덮는다: Vec4d(텍스처 설정 AlphaCoverageThresholds), Vec2, Vec2d,
// Vec4, Quatd, Rotator<float>, Rotator<double>, IntVec2.
ROCKS_STRUCT()
struct CoreValueBundle {
    ROCKS_PROPERTY() Vec4d thresholds;
    ROCKS_PROPERTY() Vec2 uv;
    ROCKS_PROPERTY() Vec2d uvPrecise;
    ROCKS_PROPERTY() Vec4 weights;
    ROCKS_PROPERTY() Quatd orientation;
    ROCKS_PROPERTY() Rotator<float> heading;
    ROCKS_PROPERTY() Rotator<double> headingPrecise;
    ROCKS_PROPERTY() IntVec2 size;
};

// B4c Core 값 타입 필드 fixture다(Docs/Design/ReflectionFoundations.ko.md 4.5절). 필드 형식은 실제
// Core 타입(별칭 포함)이고 CoreTypeBindings의 Binds 거울로 리플렉션된다: 별칭 Vec3·Quat, 비템플릿
// Color·IntVec3, 다른 거울에 의존하는 Transform, EditorOnly LinearColor, Core 값 멤버를 담는 사용자
// struct.
ROCKS_CLASS()
struct CoreValueFields final : Object {
    ROCKS_PROPERTY() Vec3 position;
    ROCKS_PROPERTY() Quat rotation;
    ROCKS_PROPERTY() Color tint;
    ROCKS_PROPERTY() Transform placement;
    ROCKS_PROPERTY() IntVec3 cell;
    ROCKS_PROPERTY(EditorOnly) LinearColor debugColor;
    ROCKS_PROPERTY() CoreValueBundle bundle;
};

} // namespace rocks::reflection_fixture
