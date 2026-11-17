#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9j InstancedStruct PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 11절).
// InstancedPoint·InstancedScale은 담을 수 있는 값 struct, InstancedVersioned는 버전 2 struct(옛
// 버전 내용의 마이그레이션 시험), InstancedRefHolder·InstancedRequiredLink·InstancedNested는 담을
// 수 없는 struct(강한 참조·Required·InstancedStruct), InstancedHost는 InstancedStruct 멤버를 담은
// struct(위치 깊이 1), InstancedDepth1..16은 깊이 합 시험의 사슬이다. InstancedHolder는 필드·원소
// 배열·정적 배열·Map 값·struct 멤버·ReadOnly 자리의 InstancedStruct를 담고 그래프
// 왕복·경로·텍스트로 시험한다(InstancedStructGraphTests·InstancedStructPathTests).
ROCKS_STRUCT()
struct InstancedPoint {
    explicit InstancedPoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() String label;
};

ROCKS_STRUCT()
struct InstancedScale {
    explicit InstancedScale(Allocator& allocator) noexcept : steps(allocator) {
    }

    ROCKS_PROPERTY() float factor = 1.0f;
    ROCKS_PROPERTY() Array<uint32> steps;
};

// 버전 2 struct다(옛 버전 1 내용 Record의 struct 마이그레이션 시험).
ROCKS_STRUCT(Version = 2)
struct InstancedVersioned {
    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() int32 extra = 0;
};

// 강한 참조를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedRefHolder {
    ROCKS_PROPERTY() ObjectRef<Object> target;
};

// Required 멤버를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedRequiredLink {
    explicit InstancedRequiredLink(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) SoftObjectRef<> asset;
};

// InstancedStruct를 담는 struct다(InstancedStruct에 담을 수 없다 — 중첩 금지).
ROCKS_STRUCT()
struct InstancedNested {
    ROCKS_PROPERTY() InstancedStruct<> inner;
};

// InstancedStruct 멤버를 담는 struct다(멤버의 위치 깊이 1).
ROCKS_STRUCT()
struct InstancedHost {
    ROCKS_PROPERTY() InstancedStruct<> value;
    ROCKS_PROPERTY() uint32 count = 0;
};

// 깊이 사슬이다(InstancedDepthN의 깊이는 N).
ROCKS_STRUCT()
struct InstancedDepth1 {
    ROCKS_PROPERTY() int32 v = 0;
};

ROCKS_STRUCT()
struct InstancedDepth2 {
    ROCKS_PROPERTY() InstancedDepth1 inner;
};

ROCKS_STRUCT()
struct InstancedDepth3 {
    ROCKS_PROPERTY() InstancedDepth2 inner;
};

ROCKS_STRUCT()
struct InstancedDepth4 {
    ROCKS_PROPERTY() InstancedDepth3 inner;
};

ROCKS_STRUCT()
struct InstancedDepth5 {
    ROCKS_PROPERTY() InstancedDepth4 inner;
};

ROCKS_STRUCT()
struct InstancedDepth6 {
    ROCKS_PROPERTY() InstancedDepth5 inner;
};

ROCKS_STRUCT()
struct InstancedDepth7 {
    ROCKS_PROPERTY() InstancedDepth6 inner;
};

ROCKS_STRUCT()
struct InstancedDepth8 {
    ROCKS_PROPERTY() InstancedDepth7 inner;
};

ROCKS_STRUCT()
struct InstancedDepth9 {
    ROCKS_PROPERTY() InstancedDepth8 inner;
};

ROCKS_STRUCT()
struct InstancedDepth10 {
    ROCKS_PROPERTY() InstancedDepth9 inner;
};

ROCKS_STRUCT()
struct InstancedDepth11 {
    ROCKS_PROPERTY() InstancedDepth10 inner;
};

ROCKS_STRUCT()
struct InstancedDepth12 {
    ROCKS_PROPERTY() InstancedDepth11 inner;
};

ROCKS_STRUCT()
struct InstancedDepth13 {
    ROCKS_PROPERTY() InstancedDepth12 inner;
};

ROCKS_STRUCT()
struct InstancedDepth14 {
    ROCKS_PROPERTY() InstancedDepth13 inner;
};

ROCKS_STRUCT()
struct InstancedDepth15 {
    ROCKS_PROPERTY() InstancedDepth14 inner;
};

ROCKS_STRUCT()
struct InstancedDepth16 {
    ROCKS_PROPERTY() InstancedDepth15 inner;
};

ROCKS_CLASS()
struct InstancedHolder final : Object {
    explicit InstancedHolder(Allocator& allocator) noexcept : list(allocator), table(allocator) {
    }

    ROCKS_PROPERTY() InstancedStruct<> value;
    ROCKS_PROPERTY() InstancedStruct<> spare;
    ROCKS_PROPERTY() Array<InstancedStruct<>> list;
    ROCKS_PROPERTY() InstancedStruct<> fixed[2];
    ROCKS_PROPERTY() OrderedMap<uint32, InstancedStruct<>> table;
    ROCKS_PROPERTY() InstancedHost host;
    ROCKS_PROPERTY(ReadOnly) InstancedStruct<> locked;
};

} // namespace rocks::reflection_fixture
