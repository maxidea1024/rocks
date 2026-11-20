#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9d2 ClassRef PROPERTY와 타입 도구 이름 fixture다(Docs/Design/ReflectionExtensions.ko.md
// 5.2절). ClassBase는 추상 클래스(ClassRef는 추상 클래스도 담는다)이고 ClassDerived·ClassLeaf는 그
// 파생, ClassStranger는 Base와 무관한 대조용 클래스, ClassValue는 struct ID 대조용 struct다.
// ClassHolder는 단일 필드(Required 포함)·자기 클래스를 Base로 둔 필드(ClassRef<ClassHolder>)·
// Array<ClassRef> 원소·ClassRef 멤버 struct(ClassSlot)와 그 원소 배열을 담고, 그래프 왕복·미등록
// 클래스 decode 정책(Reject·ReportAndNull)·Base 위반·쓰기 검사·경로 쓰기로 시험한다
// (ClassRefGraphTests·ClassRefFieldTests). ClassRenamed는 Name 지정자로 바꾼 타입 도구 이름이다.
ROCKS_CLASS(Abstract)
struct ClassBase : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_CLASS()
struct ClassDerived : ClassBase {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 extra = 0;
};

ROCKS_CLASS()
struct ClassLeaf final : ClassDerived {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct ClassStranger final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_CLASS(Name = "Fixture.Renamed")
struct ClassRenamed final : Object {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_STRUCT()
struct ClassValue {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_STRUCT()
struct ClassSlot {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ClassRef<ClassBase> kind;
    ROCKS_PROPERTY() uint32 count = 0;
};

ROCKS_CLASS()
struct ClassHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit ClassHolder(Allocator& allocator) noexcept : list(allocator), slots(allocator) {
    }

    ROCKS_PROPERTY() ClassRef<ClassBase> any;
    ROCKS_PROPERTY(Required) ClassRef<ClassBase> required;
    ROCKS_PROPERTY() ClassRef<ClassDerived> derived;
    ROCKS_PROPERTY() ClassRef<ClassHolder> self;
    ROCKS_PROPERTY() Array<ClassRef<ClassBase>> list;
    ROCKS_PROPERTY() ClassSlot slot;
    ROCKS_PROPERTY() Array<ClassSlot> slots;
    ROCKS_PROPERTY() ClassValue plain;
};

} // namespace rocks::reflection_fixture
