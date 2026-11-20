#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9c2 Name 필드 fixture다(Docs/Design/ReflectionExtensions.ko.md 4·4.1절). Name 클래스
// 필드·struct 멤버·Array<Name> 원소(클래스 필드와 struct 멤버)와 struct 원소 배열을 MetaCompiler
// 생성 코드로 그래프 왕복(표시·번호 접미·None)·2a intern·pool 검사·경로 쓰기로 시험한다
// (NameGraphTests). NameSocket은 Name을 직접, NameGroup은 원소 struct로 간접 담는다(관찰 struct
// 제한). NamePlain은 Name이 없는 대조용 struct이고, texts는 같은 wire(String 원소 ValueArray)의
// 작업량 대조용 String 배열이다. NameWrapper는 Name struct를 일반(배열 아닌) 멤버로 담아 형질
// ContainsName의 멤버 struct 전파를 본다. NameVersioned·NameMigrated(버전 2)는 노드·struct
// 마이그레이션 결과에서 읽은 Name(1단계가 보관본을 빌리고 2단계가 같은 순서로 소비)을 본다.
ROCKS_STRUCT()
struct NameSocket {
    ROCKS_GENERATED_BODY()

    explicit NameSocket(Allocator& allocator) noexcept : aliases(allocator) {
    }

    ROCKS_PROPERTY() Name socket;
    ROCKS_PROPERTY() Array<Name> aliases;
    ROCKS_PROPERTY() uint32 slot = 0;
};

ROCKS_STRUCT()
struct NameGroup {
    ROCKS_GENERATED_BODY()

    explicit NameGroup(Allocator& allocator) noexcept : sockets(allocator) {
    }

    ROCKS_PROPERTY() Array<NameSocket> sockets;
};

ROCKS_STRUCT()
struct NamePlain {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_STRUCT()
struct NameWrapper {
    ROCKS_GENERATED_BODY()

    explicit NameWrapper(Allocator& allocator) noexcept : socket(allocator) {
    }

    ROCKS_PROPERTY() NameSocket socket;
};

ROCKS_STRUCT(Version = 2)
struct NameVersioned {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name tag;
    ROCKS_PROPERTY() uint32 extra = 0;
};

ROCKS_CLASS(Version = 2)
struct NameMigrated final : Object {
    ROCKS_GENERATED_BODY()

    explicit NameMigrated(Allocator& allocator) noexcept : names(allocator) {
    }

    ROCKS_PROPERTY() Name label;
    ROCKS_PROPERTY() NameVersioned versioned;
    ROCKS_PROPERTY() Array<Name> names;
};

ROCKS_CLASS()
struct NameFields final : Object {
    ROCKS_GENERATED_BODY()

    explicit NameFields(Allocator& allocator) noexcept
        : socket(allocator), names(allocator), group(allocator), texts(allocator) {
    }

    ROCKS_PROPERTY() Name label;
    ROCKS_PROPERTY() Name other;
    ROCKS_PROPERTY() NameSocket socket;
    ROCKS_PROPERTY() Array<Name> names;
    ROCKS_PROPERTY() NameGroup group;
    ROCKS_PROPERTY() NamePlain plain;
    ROCKS_PROPERTY() Array<String> texts;
};

} // namespace rocks::reflection_fixture
