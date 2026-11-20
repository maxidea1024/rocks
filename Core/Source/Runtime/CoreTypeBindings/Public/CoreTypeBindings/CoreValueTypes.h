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
// 미러 17종 중 혼합 정밀도 Transform을 뺀 16종은 Atomic(통째 단위 — TypeFlags::Atomic,
// Docs/Design/ReflectionExtensions.ko.md 3.1·3.2절)이다: 차이 저장은 성분 하나라도 기준과
// 다르면 모든 성분을 쓰고, override 집합·되돌리기의 단위가 그 값 필드 경로다(차이 쓰기 구현은
// A5). UE가 수학 타입을 native Serializer나 Immutable 이진 직렬화로 통째 쓰는 결과를 따른다.
// Transform은 UE FTransform처럼 비Atomic(멤버 단위)이라 prefab·instance override에서 이동만 덮고
// 회전·크기는 기준 변경을 따라갈 수 있다 — 멤버 Vec3d·Quat·Vec3는 각각 Atomic이라 멤버마다
// 통째다. 로드·경로 접근은 Atomic과 무관하게 멤버 단위다.
//
// 사용 예(이 모듈에 의존하는 모듈의 리플렉션 클래스):
//   #include <Core/Math/Transform.h>
//   ROCKS_CLASS()
//   struct Marker : rocks::Object {
//       ROCKS_GENERATED_BODY()
//       ROCKS_PROPERTY() rocks::Vec3 offset;           // 미러 Vec3Binding으로 리플렉션된다
//       ROCKS_PROPERTY() rocks::Transform placement;    // 중첩 미러(Vec3d·Quat·Vec3)
//       ROCKS_PROPERTY() rocks::Transform3f pose;       // float TRS(Atomic)
//   };
// 미러 타입(…Binding)을 필드 형식으로 직접 쓰지 않는다 — 실제 타입을 쓴다.
namespace rocks::core_type_bindings {

ROCKS_STRUCT(Binds = "rocks::Vec2", Atomic, Version = 1)
struct Vec2Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec2d", Atomic, Version = 1)
struct Vec2dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
};

// 3D 벡터다. 공간 위치·방향으로 쓰면 UE 5.8과 같은 왼손 +X 전방·+Y 오른쪽·+Z 위, 길이 cm다
// (Core/Math/MathUtility.h 머리 주석). 저장 형식에는 단위를 기록하지 않으며 필드 값을 바꾸지 않고
// UE FVector3f·FVector 값과 그대로 대응한다.
ROCKS_STRUCT(Binds = "rocks::Vec3", Atomic, Version = 1)
struct Vec3Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec3d", Atomic, Version = 1)
struct Vec3dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec4", Atomic, Version = 1)
struct Vec4Binding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
    ROCKS_PROPERTY() float w = 0;
};

ROCKS_STRUCT(Binds = "rocks::Vec4d", Atomic, Version = 1)
struct Vec4dBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
    ROCKS_PROPERTY() double w = 0;
};

// quaternion이다. 성분 순서 (x, y, z, w)와 회전 의미(양의 각은 왼손 회전)가 UE FQuat와 같다
// (Core/Math/Quaternion.h).
ROCKS_STRUCT(Binds = "rocks::Quat", Atomic, Version = 1)
struct QuatBinding {
    ROCKS_PROPERTY() float x = 0;
    ROCKS_PROPERTY() float y = 0;
    ROCKS_PROPERTY() float z = 0;
    ROCKS_PROPERTY() float w = 1;
};

ROCKS_STRUCT(Binds = "rocks::Quatd", Atomic, Version = 1)
struct QuatdBinding {
    ROCKS_PROPERTY() double x = 0;
    ROCKS_PROPERTY() double y = 0;
    ROCKS_PROPERTY() double z = 0;
    ROCKS_PROPERTY() double w = 1;
};

// 회전은 pitch·yaw·roll 도(degree)다. UE FRotator와 필드 순서·단위·의미가 같다
// (Core/Math/Rotator.h).
ROCKS_STRUCT(Binds = "rocks::Rotator<float>", Atomic, Version = 1)
struct RotatorBinding {
    ROCKS_PROPERTY() float pitch = 0;
    ROCKS_PROPERTY() float yaw = 0;
    ROCKS_PROPERTY() float roll = 0;
};

ROCKS_STRUCT(Binds = "rocks::Rotator<double>", Atomic, Version = 1)
struct RotatordBinding {
    ROCKS_PROPERTY() double pitch = 0;
    ROCKS_PROPERTY() double yaw = 0;
    ROCKS_PROPERTY() double roll = 0;
};

// 8비트 unorm RGBA 색이다(색 공간은 기록하지 않는다 — Color.h).
ROCKS_STRUCT(Binds = "rocks::Color", Atomic, Version = 1)
struct ColorBinding {
    ROCKS_PROPERTY() uint8 r = 0;
    ROCKS_PROPERTY() uint8 g = 0;
    ROCKS_PROPERTY() uint8 b = 0;
    ROCKS_PROPERTY() uint8 a = 0;
};

// 선형 float 색(RGBA, straight alpha)이다.
ROCKS_STRUCT(Binds = "rocks::LinearColor", Atomic, Version = 1)
struct LinearColorBinding {
    ROCKS_PROPERTY() float r = 0;
    ROCKS_PROPERTY() float g = 0;
    ROCKS_PROPERTY() float b = 0;
    ROCKS_PROPERTY() float a = 0;
};

ROCKS_STRUCT(Binds = "rocks::IntVec2", Atomic, Version = 1)
struct IntVec2Binding {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

ROCKS_STRUCT(Binds = "rocks::IntVec3", Atomic, Version = 1)
struct IntVec3Binding {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() int32 z = 0;
};

// 회전·이동·축별 scale이 모두 double인 TRS(rocks::Transform — UE FTransform)다. 멤버 순서는 UE
// FTransform과 같은 rotation·translation·scale이다(Core/Math/Transform.h). 멤버는 위 미러들이
// 결합한 실제 타입이다. 비Atomic(멤버 단위 — UE FTransform과 같다)이다. 세이브게임 포함
// 필터(includeFlags = SaveGame)는 멤버마다 자기 플래그를 보므로 멤버에 SaveGame을 단다(UE
// FTransform 멤버와 같다 — 리플렉션 B9h1). Atomic 미러의 멤버는 통째 단위라 표시하지 않는다. 필드
// ID는 이름 기반이라 순서· 정밀도를 바꿔도 그대로이고, 저장된 값이 없어 Version 1을 유지한다.
// translation은 cm, scale은 무차원 배율이다(UE FTransform과 같다).
ROCKS_STRUCT(Binds = "rocks::Transform", Version = 1)
struct TransformBinding {
    ROCKS_PROPERTY(SaveGame) Quatd rotation;
    ROCKS_PROPERTY(SaveGame) Vec3d translation;
    ROCKS_PROPERTY(SaveGame) Vec3d scale = Vec3d::One();
};

// 모든 성분이 float인 TRS(rocks::Transform3<float> — UE FTransform3f)다. 멤버 순서는 Core·UE 선언
// 순서 rotation·translation·scale이다(UE FTransform3f는 Rotation·Translation·Scale3D 순서의
// immutable 이진이라 반입은 멤버 이름으로 대응시킨다 — Transform.h 머리 주석).
ROCKS_STRUCT(Binds = "rocks::Transform3f", Atomic, Version = 1)
struct Transform3fBinding {
    ROCKS_PROPERTY() Quat rotation;
    ROCKS_PROPERTY() Vec3 translation;
    ROCKS_PROPERTY() Vec3 scale = Vec3::One();
};

// 모든 성분이 double인 TRS(rocks::Transform3<double> — UE FTransform3d)다. 멤버 순서는
// Transform3fBinding과 같다.
ROCKS_STRUCT(Binds = "rocks::Transform3d", Atomic, Version = 1)
struct Transform3dBinding {
    ROCKS_PROPERTY() Quatd rotation;
    ROCKS_PROPERTY() Vec3d translation;
    ROCKS_PROPERTY() Vec3d scale = Vec3d::One();
};

} // namespace rocks::core_type_bindings
