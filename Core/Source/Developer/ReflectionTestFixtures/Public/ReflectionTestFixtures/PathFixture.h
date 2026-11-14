#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>

namespace rocks::reflection_fixture {

// B6 경로 접근 fixture다(Docs/Design/ReflectionFoundations.ko.md 7.1절). MetaCompiler 생성 경로
// thunk로 도구 이름(Name 지정자)을 바꾼 struct 필드·배열 필드·struct 멤버의 문자열 경로 해석(C++
// 이름은 받지 않는다)과 ReadOnly struct 필드·배열 필드(값·struct 원소·참조)·struct 멤버의 전파를
// 시험한다(GeneratedPathTests).
ROCKS_STRUCT()
struct PathEntry {
    explicit PathEntry(Allocator& allocator) noexcept : caption(allocator) {
    }

    ROCKS_PROPERTY(Name = "Title") String caption;
    ROCKS_PROPERTY(ReadOnly) uint32 serial = 0;
    ROCKS_PROPERTY() int32 weight = 0;
};

ROCKS_CLASS()
struct PathOwner final : Object {
    explicit PathOwner(Allocator& allocator) noexcept
        : primary(allocator), frozen(allocator), locked(allocator), entries(allocator),
          frozenEntries(allocator), frozenLinks(allocator) {
    }

    ROCKS_PROPERTY(Name = "Main") PathEntry primary;
    // 바깥 ReadOnly가 멤버 쓰기를 모두 막는다(읽기는 된다).
    ROCKS_PROPERTY(ReadOnly) PathEntry frozen;
    // 원소 교체·삽입·삭제·비우기를 모두 막는다.
    ROCKS_PROPERTY(ReadOnly) Array<uint32> locked;
    ROCKS_PROPERTY(Name = "Items") Array<PathEntry> entries;
    // struct 원소 삽입·삭제·비우기와 원소 멤버 쓰기를 모두 막는다.
    ROCKS_PROPERTY(ReadOnly) Array<PathEntry> frozenEntries;
    // 참조 원소 교체·삽입·삭제·비우기를 모두 막는다(Registry 컨테이너).
    ROCKS_PROPERTY(ReadOnly) ObjectRefArray<PathOwner> frozenLinks;
};

} // namespace rocks::reflection_fixture
