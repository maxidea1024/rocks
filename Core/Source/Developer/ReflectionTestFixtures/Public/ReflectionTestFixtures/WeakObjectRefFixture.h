#pragma once

#include <Core/Containers/Array.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9d1 WeakObjectRef PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 5.1절).
// weak 클래스 필드(기대 타입 WeakTarget·Object)·struct 멤버(WeakLink)·Array<WeakObjectRef>
// 원소·weak 멤버 struct의 원소 배열(WeakLinkGroup — 형질 ContainsWeakReference의 원소 struct
// 전파)을 MetaCompiler 생성 코드로 그래프 왕복·저장 정책별 처리(Reject·Admit·Include 지연
// 덮어쓰기)·쓰기 검증·경로 쓰기·GC로
// 시험한다(WeakObjectRefGraphTests·WeakObjectRefGraphLoadTests·WeakObjectRefFieldTests).
// WeakHolder는 WeakTarget의 파생이라 holder끼리 weak로 가리킬 수 있고, strong·second는 Include
// 집합을 넓히는 강한 참조다. WeakOther는 기대 타입과 무관한 대조용 클래스다.
ROCKS_CLASS()
struct WeakTarget : Object {
    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_CLASS()
struct WeakOther final : Object {
    ROCKS_PROPERTY() uint32 value = 0;
};

ROCKS_STRUCT()
struct WeakLink {
    ROCKS_PROPERTY() WeakObjectRef<WeakTarget> target;
    ROCKS_PROPERTY() uint32 slot = 0;
};

ROCKS_STRUCT()
struct WeakLinkGroup {
    explicit WeakLinkGroup(Allocator& allocator) noexcept : links(allocator) {
    }

    ROCKS_PROPERTY() Array<WeakLink> links;
};

ROCKS_CLASS()
struct WeakHolder final : WeakTarget {
    explicit WeakHolder(Allocator& allocator) noexcept : targets(allocator), group(allocator) {
    }

    ROCKS_PROPERTY() WeakObjectRef<WeakTarget> target;
    ROCKS_PROPERTY() WeakObjectRef<> any;
    ROCKS_PROPERTY() ObjectRef<WeakTarget> strong;
    ROCKS_PROPERTY() ObjectRef<WeakTarget> second;
    ROCKS_PROPERTY() WeakLink link;
    ROCKS_PROPERTY() Array<WeakObjectRef<WeakTarget>> targets;
    ROCKS_PROPERTY() WeakLinkGroup group;
};

} // namespace rocks::reflection_fixture
