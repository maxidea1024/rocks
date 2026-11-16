#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9h2 클래스 플래그 fixture다(Docs/Design/ReflectionExtensions.ko.md 9.2절). KindBase
// 아래 Transient 클래스(TransientThing — 파생 TransientChild가 상속한다)와 Deprecated 클래스
// (DeprecatedThing), 그 객체를 가리키는 참조·ClassRef 필드를 담은 ClassFlagHolder(저장 정책
// Exclude·Keep, 로드 진단, ClassRef 쓰기 거부·decode 진단), Within 클래스(WithinInner — Outer가
// WithinOuter 또는 파생이어야 한다, 파생 WithinInnerChild가 이어받는다)를 담는다
// (ClassFlagGraphTests).
ROCKS_CLASS()
struct KindBase : Object {};

ROCKS_CLASS(Transient)
struct TransientThing : KindBase {};

ROCKS_CLASS()
struct TransientChild final : TransientThing {};

ROCKS_CLASS(Deprecated)
struct DeprecatedThing final : KindBase {};

ROCKS_CLASS()
struct ClassFlagHolder final : Object {
    explicit ClassFlagHolder(Allocator& allocator) noexcept : kinds(allocator) {
    }

    ROCKS_PROPERTY() ObjectRef<Object> strong;
    ROCKS_PROPERTY() WeakRef<Object> weak;
    ROCKS_PROPERTY(Required) ObjectRef<Object> needed;
    ROCKS_PROPERTY() ClassRef<KindBase> kind;
    ROCKS_PROPERTY() Array<ClassRef<KindBase>> kinds;
};

ROCKS_CLASS()
struct WithinOuter : Object {};

ROCKS_CLASS()
struct WithinOuterChild final : WithinOuter {};

ROCKS_CLASS(Within = "rocks::reflection_fixture::WithinOuter")
struct WithinInner : Object {};

ROCKS_CLASS()
struct WithinInnerChild final : WithinInner {};

} // namespace rocks::reflection_fixture
