#pragma once

#include <ReflectionTestFixtures/FunctionFixture.h>
#include <Base/Compiler.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Event.h>
#include <Object/Object.h>

// 리플렉션 이벤트 필드 시험 대상이다(Docs/Design/EventReflection.ko.md EV2b — ED14·ED18). 값 필드와
// 이벤트 필드가 섞인 클래스(인자 없음·값·const String&·struct·enum·ObjectHandle 인자,
// Transient·SaveGame·EditorOnly 이벤트)와 이벤트를 물려받고 하나 더하는 파생이다. 생성
// schema·연산표와 값 경로(저장·클래스 기본값·텍스트·재정의 묶음·로그 방문)가 이벤트 필드를
// 건너뛰는지 본다(EventFieldGeneratedTests).
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class EventPublisher : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit EventPublisher(Allocator& allocator) noexcept
        : label(allocator), changed(allocator), named(allocator), pinged(allocator),
          placed(allocator)
#if ROCKS_WITH_EDITOR_DATA
          ,
          edited(allocator)
#endif
    {
    }

    ROCKS_PROPERTY() int32 count = 0;
    ROCKS_PROPERTY() String label;
    // 강한 참조로 닿는 listener다(LoadObjectGraph 왕복·Include 지연 덮어쓰기 시험 — EV3a1).
    ROCKS_PROPERTY() ObjectRef<CallTarget> partner;
    ROCKS_PROPERTY(Params = "amount") Event<int32> changed;
    ROCKS_PROPERTY(Params = "text,times", Transient) Event<const String&, int32> named;
    ROCKS_PROPERTY(SaveGame) Event<> pinged;
    ROCKS_PROPERTY(Params = "point,mode,who")
    Event<const CallPoint&, CallMode, ObjectHandle> placed;
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly, Params = "amount") Event<int32> edited;
#endif
};

// 기반 이벤트를 물려받고 Transient 이벤트 하나를 더한다(파생 schema가 상속 이벤트를 다시 싣는다 —
// Transient 이벤트의 바인딩은 저장하지 않는다).
ROCKS_CLASS()
class EventPublisherChild : public EventPublisher {
    ROCKS_GENERATED_BODY()

public:
    explicit EventPublisherChild(Allocator& allocator) noexcept
        : EventPublisher(allocator), extra(allocator) {
    }

    ROCKS_PROPERTY(Params = "amount", Transient) Event<int32> extra;
};

} // namespace rocks::reflection_fixture
