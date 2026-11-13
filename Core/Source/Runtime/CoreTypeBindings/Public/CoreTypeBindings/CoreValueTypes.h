#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>
#include <Meta/Annotations.h>

// Core 값 타입의 리플렉션 미러 선언이다(Docs/Design/ReflectionFoundations.ko.md 4.5절, UE
// NoExportTypes 방식). 각 미러는 ROCKS_STRUCT(Binds = "<실제 타입>")로 실제 Core 타입의 공개 데이터
// 멤버를 같은 이름·형식·선언 순서로 나열한다(Binds 철자는 전역 기준 완전 한정 이름이다). 생성
// 코드는 미러가 아니라 실제 타입(예: rocks::Vector3<float>)의 GeneratedType을 내고, 멤버
// 이름, 형식, 개수, 순서(같은 형식 멤버끼리의 순서 포함)가 실제 타입과 다르거나 멤버가 base에
// 있으면 생성 헤더가 컴파일되지 않는다. 스키마 이름은 미러 이름이다. 저장은 다른 struct와 같은
// 태그형 중첩 Record다(4.4절).
//
// 사용 예(이 모듈에 의존하는 모듈의 리플렉션 클래스):
//   #include <Core/Math/Transform.h>
//   ROCKS_CLASS()
//   struct Marker : rocks::Object {
//       ROCKS_PROPERTY() rocks::Vec3 offset;           // 미러 Vec3Binding으로 리플렉션된다
//       ROCKS_PROPERTY() rocks::Transform placement;    // 중첩 미러(Vec3d·Quat·Vec3)
//   };
// 미러 타입(…Binding)을 필드 형식으로 직접 쓰지 않는다 — 실제 타입을 쓴다.
namespace rocks::core_type_bindings {

ROCKS_STRUCT(Binds = "rocks::Vec2", Version = 1)
struct Vec2Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec2d", Version = 1)
struct Vec2dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec3", Version = 1)
struct Vec3Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec3d", Version = 1)
struct Vec3dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec4", Version = 1)
struct Vec4Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
    ROCKS_PROPERTY() float w = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec4d", Version = 1)
struct Vec4dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
    ROCKS_PROPERTY() double w = 0;
};

ROCKS_STRUCT(Binds = "rocks::Quat", Version = 1)
struct QuatBinding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
    ROCKS_PROPERTY() float w = 1;
};

ROCKS_STRUCT(Binds = "rocks::Quatd", Version = 1)
struct QuatdBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
    ROCKS_PROPERTY() double w = 1;
};

// 회전은 yaw·pitch·roll 라디안이다(UE FRotator는 pitch·yaw·roll 도 — UE 데이터 반입 때 변환한다).
ROCKS_STRUCT(Binds = "rocks::Rotator<float>", Version = 1)
struct RotatorBinding {
    ROCKS_PROPERTY() float yaw = 0;
    ROCKS_PROPERTY() float pitch = 0;
    ROCKS_PROPERTY() float roll = 0;
};

ROCKS_STRUCT(Binds = "rocks::Rotator<double>", Version = 1)
struct RotatordBinding {
    ROCKS_PROPERTY() double yaw = 0;
    ROCKS_PROPERTY() double pitch = 0;
    ROCKS_PROPERTY() double roll = 0;
};

// 8비트 unorm RGBA 색이다(색 공간은 기록하지 않는다 — Color.h).
ROCKS_STRUCT(Binds = "rocks::Color", Version = 1)
struct ColorBinding {
    ROCKS_PROPERTY() uint8 r = 0;
    ROCKS_PROPERTY() uint8 g = 0;
    ROCKS_PROPERTY() uint8 b = 0;
    ROCKS_PROPERTY() uint8 a = 0;
};

// 선형 float 색(RGBA, straight alpha)이다.
ROCKS_STRUCT(Binds = "rocks::LinearColor", Version = 1)
struct LinearColorBinding {
    ROCKS_PROPERTY() float r = 0;
    ROCKS_PROPERTY() float g = 0;
    ROCKS_PROPERTY() float b = 0;
    ROCKS_PROPERTY() float a = 0;
};

ROCKS_STRUCT(Binds = "rocks::IntVec2", Version = 1)
struct IntVec2Binding {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

ROCKS_STRUCT(Binds = "rocks::IntVec3", Version = 1)
struct IntVec3Binding {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() int32 z = 0;
};

// 이동(double)·회전(float quaternion)·축별 scale(float)이다. 멤버는 위 미러들이 결합한 실제
// 타입이다.
ROCKS_STRUCT(Binds = "rocks::Transform", Version = 1)
struct TransformBinding {
    ROCKS_PROPERTY() Vec3d translation;
    ROCKS_PROPERTY() Quat rotation;
    ROCKS_PROPERTY() Vec3 scale = Vec3::One();
};

} // namespace rocks::core_type_bindings
