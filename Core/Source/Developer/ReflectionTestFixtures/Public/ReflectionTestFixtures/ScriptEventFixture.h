#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/Event.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>

// Sparks에 노출하는 이벤트 시험 대상이다(Docs/Design/SparksBinding.ko.md EI1c B29): ScriptType
// 클래스 하나와 ScriptAssignable 이벤트 넷(정수 인자, 문자열과 노출 enum 인자, 인자 없음, 자기
// 클래스 약한 참조)과 노출하지 않은 이벤트 하나다. 생성
// schema(ReflectionTestFixtures.sparkbind.json)로 Sparks의 `on`·`await`를 컴파일한다
// (SparksToolsBindingTests). 타입 이름은 World·Gameplay 타입과 겹치지 않는다(겹치면 schema reader가
// 파일 하나를 S1120으로 버린다).
namespace rocks::reflection_fixture {

ROCKS_ENUM(ScriptType)
enum class BeaconMood : uint8 {
    Calm,
    Alert
};

ROCKS_CLASS(ScriptType)
class EventBeacon : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit EventBeacon(Allocator& allocator) noexcept
        : scored(allocator), tagged(allocator), pinged(allocator), linked(allocator),
          hidden(allocator) {
    }

    ROCKS_PROPERTY(Params = "amount", ScriptAssignable) Event<int32> scored;
    ROCKS_PROPERTY(Params = "label, mood", ScriptAssignable)
    Event<const String&, BeaconMood> tagged;
    ROCKS_PROPERTY(ScriptAssignable) Event<> pinged;
    ROCKS_PROPERTY(Params = "other", ScriptAssignable) Event<WeakObjectRef<EventBeacon>> linked;
    // 노출하지 않은 이벤트는 schema에 없다.
    ROCKS_PROPERTY(Params = "amount") Event<int32> hidden;
};

} // namespace rocks::reflection_fixture
