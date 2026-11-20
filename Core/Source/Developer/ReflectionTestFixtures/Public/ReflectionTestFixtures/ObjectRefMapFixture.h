#pragma once

#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9f4 ObjectRefMap PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 7.4절).
// RefMapTarget·RefMapDerived는 값의 대상 클래스(파생 포함)이고 RefMapOther는 대상 타입이 아닌
// 클래스(TypeMismatch 시험)다. RefMapSlot은 enum 키, RefMapBundle은 ObjectRefMap을 담는 인라인
// struct(참조 struct이고 Registry 컨테이너를 담는다 — StructRefArray 원소가 될 수 없다)다.
// RefMapHolder는 키 종류별 ObjectRefMap(String·uint32 — 기본 대상 Object·enum·ClassRef·Soft),
// Required 값 Map, 상한 2의 Map, 인라인 struct를 담고 그래프 왕복·Trace·Required·상한·Commit 검증·
// 경로 연산으로 시험한다(ObjectRefMapGraphTests·ObjectRefMapPathTests).
ROCKS_ENUM()
enum class RefMapSlot : uint8 {
    Head = 1,
    Hand = 2,
    Feet = 3
};

ROCKS_CLASS()
struct RefMapTarget : Object {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct RefMapDerived final : RefMapTarget {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct RefMapOther final : Object {
    ROCKS_GENERATED_BODY()
};

ROCKS_STRUCT()
struct RefMapBundle {
    ROCKS_GENERATED_BODY()

    explicit RefMapBundle(Allocator& allocator) noexcept : byName(allocator) {
    }

    ROCKS_PROPERTY() ObjectRefMap<Name, RefMapTarget> byName;
    ROCKS_PROPERTY() uint32 weight = 0;
};

ROCKS_CLASS()
struct RefMapHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit RefMapHolder(Allocator& allocator) noexcept
        : byString(allocator), byNumber(allocator), bySlot(allocator), byClass(allocator),
          bySoft(allocator), required(allocator), limited(allocator), bundle(allocator) {
    }

    ROCKS_PROPERTY() ObjectRefMap<String, RefMapTarget> byString;
    ROCKS_PROPERTY() ObjectRefMap<uint32> byNumber;
    ROCKS_PROPERTY() ObjectRefMap<RefMapSlot, RefMapTarget> bySlot;
    ROCKS_PROPERTY() ObjectRefMap<ClassRef<RefMapTarget>, RefMapTarget> byClass;
    ROCKS_PROPERTY() ObjectRefMap<SoftObjectRef<>, RefMapTarget> bySoft;
    ROCKS_PROPERTY(Required) ObjectRefMap<uint32, RefMapTarget> required;
    ROCKS_PROPERTY(MaxElements = 2) ObjectRefMap<uint32, RefMapTarget> limited;
    ROCKS_PROPERTY() RefMapBundle bundle;
};

} // namespace rocks::reflection_fixture
