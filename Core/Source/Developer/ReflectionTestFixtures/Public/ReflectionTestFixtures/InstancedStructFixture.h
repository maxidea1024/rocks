#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
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
    ROCKS_GENERATED_BODY()

    explicit InstancedPoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() String label;
};

ROCKS_STRUCT()
struct InstancedScale {
    ROCKS_GENERATED_BODY()

    explicit InstancedScale(Allocator& allocator) noexcept : steps(allocator) {
    }

    ROCKS_PROPERTY() float factor = 1.0f;
    ROCKS_PROPERTY() Array<uint32> steps;
};

// 버전 2 struct다(옛 버전 1 내용 Record의 struct 마이그레이션 시험).
ROCKS_STRUCT(Version = 2)
struct InstancedVersioned {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() int32 extra = 0;
};

// 강한 참조를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedRefHolder {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ObjectRef<Object> target;
};

// Required 멤버를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedRequiredLink {
    ROCKS_GENERATED_BODY()

    explicit InstancedRequiredLink(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) SoftObjectRef<> asset;
};

// InstancedStruct를 담는 struct다(InstancedStruct에 담을 수 없다 — 중첩 금지).
ROCKS_STRUCT()
struct InstancedNested {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedStruct<> inner;
};

// Registry 컨테이너(ObjectRefArray)를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedRefArrayHolder {
    ROCKS_GENERATED_BODY()

    explicit InstancedRefArrayHolder(Allocator& allocator) noexcept : refs(allocator) {
    }

    ROCKS_PROPERTY() ObjectRefArray<Object> refs;
};

// WorldOnly 소유자를 요구하는 NATIVE 타입이다.
struct InstancedWorldToken {
    static constexpr bool RocksRequiresWorldOnlyOwner = true;
    int32 value = 0;
};

// WorldOnly 소유자를 요구하는 NATIVE 멤버를 담는 struct다(InstancedStruct에 담을 수 없다).
ROCKS_STRUCT()
struct InstancedWorldBound {
    ROCKS_GENERATED_BODY()

    ROCKS_NATIVE() InstancedWorldToken token;
    ROCKS_PROPERTY() int32 v = 0;
};

// ReadOnly 멤버를 담는 struct다(InstancedStruct 내용 안 ReadOnly 대입 — B9j–B9m 묶음 리뷰 I3).
ROCKS_STRUCT()
struct InstancedLocked {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY(ReadOnly) int32 fixed = 0;
    ROCKS_PROPERTY() int32 free = 0;
};

// InstancedStruct 멤버를 담는 struct다(멤버의 위치 깊이 1).
ROCKS_STRUCT()
struct InstancedHost {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedStruct<> value;
    ROCKS_PROPERTY() uint32 count = 0;
};

// 깊이 사슬이다(InstancedDepthN의 깊이는 N).
ROCKS_STRUCT()
struct InstancedDepth1 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 v = 0;
};

ROCKS_STRUCT()
struct InstancedDepth2 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth1 inner;
};

ROCKS_STRUCT()
struct InstancedDepth3 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth2 inner;
};

ROCKS_STRUCT()
struct InstancedDepth4 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth3 inner;
};

ROCKS_STRUCT()
struct InstancedDepth5 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth4 inner;
};

ROCKS_STRUCT()
struct InstancedDepth6 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth5 inner;
};

ROCKS_STRUCT()
struct InstancedDepth7 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth6 inner;
};

ROCKS_STRUCT()
struct InstancedDepth8 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth7 inner;
};

ROCKS_STRUCT()
struct InstancedDepth9 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth8 inner;
};

ROCKS_STRUCT()
struct InstancedDepth10 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth9 inner;
};

ROCKS_STRUCT()
struct InstancedDepth11 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth10 inner;
};

ROCKS_STRUCT()
struct InstancedDepth12 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth11 inner;
};

ROCKS_STRUCT()
struct InstancedDepth13 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth12 inner;
};

ROCKS_STRUCT()
struct InstancedDepth14 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth13 inner;
};

ROCKS_STRUCT()
struct InstancedDepth15 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth14 inner;
};

ROCKS_STRUCT()
struct InstancedDepth16 {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() InstancedDepth15 inner;
};

ROCKS_CLASS()
struct InstancedHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit InstancedHolder(Allocator& allocator) noexcept
        : list(allocator), table(allocator), hosts(allocator) {
    }

    ROCKS_PROPERTY() InstancedStruct<> value;
    ROCKS_PROPERTY() InstancedStruct<> spare;
    ROCKS_PROPERTY() Array<InstancedStruct<>> list;
    ROCKS_PROPERTY() InstancedStruct<> fixed[2];
    ROCKS_PROPERTY() OrderedMap<uint32, InstancedStruct<>> table;
    ROCKS_PROPERTY() InstancedHost host;
    ROCKS_PROPERTY(ReadOnly) InstancedStruct<> locked;
    // struct 원소 배열 안 InstancedStruct 멤버(위치 깊이 1 — 대입 묶음 상자의 깊이 합, 리뷰 S1)다.
    ROCKS_PROPERTY() Array<InstancedHost> hosts;
    // 정적 배열 struct 원소 안 InstancedStruct 멤버(값 단위가 원소 멤버 — 상자 시작 위치가 깊이
    // 1)다.
    ROCKS_PROPERTY() InstancedHost slots[2];
};

} // namespace rocks::reflection_fixture
