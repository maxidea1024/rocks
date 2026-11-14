#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <ReflectionTestFixtures/ReflectionFixture.h>

namespace rocks::reflection_fixture {

// B4c 사용자 리플렉션 struct가 Core 값 타입 멤버를 담는 경우다(멤버 struct가 다른 모듈의 Binds
// 미러). 나머지 미러 타입을 모두 덮는다: Vec4d(텍스처 설정 AlphaCoverageThresholds), Vec2, Vec2d,
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
// Core 타입(별칭 포함)이고 CoreTypeBindings의 Binds 미러로 리플렉션된다: 별칭 Vec3·Quat, 비템플릿
// Color·IntVec3, 다른 미러에 의존하는 Transform, EditorOnly LinearColor, Core 값 멤버를 담는 사용자
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

// B8a native 파생 시험의 SceneComponent형 리플렉션 base다(Docs/Design/ReflectionFoundations.ko.md
// 9.1절). 다른 헤더의 WorldOnly 클래스(WorldFixture)를 상속해 WorldOnly를 다시 쓰지 않고, 다른 모듈
// Binds 미러 struct 필드(Transform)를 더한다. 시험 TU의 native 파생이 이 두 단계 리플렉션 사슬의
// 필드(struct 의존 포함)를 빌린다.
ROCKS_CLASS()
struct SceneFixture : WorldFixture {
    ROCKS_PROPERTY() Transform relative;
};

} // namespace rocks::reflection_fixture
