#pragma once

#include <Meta/Annotations.h>
#include <ReflectionTestFixtures/EventRelayFixture.h>

// 상속 이벤트를 선언한 조상(EventPublisher — EventFixture.h)이 바로 위 base(EventRelay —
// EventRelayFixture.h)와 다른 헤더에 있는 파생이다(메타 컴파일러 회귀 — Docs/Design/PhysicsModule
// 7.1.14절). 이 헤더의 생성 소스는 base 헤더만으로는 EventPublisher의 생성 헤더가 보이지 않아,
// 이벤트 선언 클래스의 생성 헤더를 따로 include해야 컴파일된다.
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class EventRelayLeaf : public EventRelay {
    ROCKS_GENERATED_BODY()

public:
    explicit EventRelayLeaf(Allocator& allocator) noexcept : EventRelay(allocator) {
    }
};

} // namespace rocks::reflection_fixture
