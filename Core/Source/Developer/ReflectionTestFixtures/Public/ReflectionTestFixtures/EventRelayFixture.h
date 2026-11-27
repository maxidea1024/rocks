#pragma once

#include <Meta/Annotations.h>
#include <ReflectionTestFixtures/EventFixture.h>

// 상속 이벤트를 선언한 조상이 다른 헤더에 있는 파생 사슬의 중간이다(메타 컴파일러 회귀 —
// Docs/Design/PhysicsModule 7.1.14절). EventRelayLeafFixture.h가 이 클래스를 물려받는다.
namespace rocks::reflection_fixture {

ROCKS_CLASS()
class EventRelay : public EventPublisherChild {
    ROCKS_GENERATED_BODY()

public:
    explicit EventRelay(Allocator& allocator) noexcept : EventPublisherChild(allocator) {
    }
};

} // namespace rocks::reflection_fixture
