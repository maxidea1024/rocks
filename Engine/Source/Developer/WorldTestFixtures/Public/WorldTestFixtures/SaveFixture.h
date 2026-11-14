#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>
#include <World/Actor.h>
#include <World/SceneComponent.h>

namespace rocks::world_fixture {

// World 리플렉션 시험 fixture다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8b). 다른
// 모듈(World)의 WorldOnly 리플렉션 클래스를 상속해 그 상속 필드(components·rootComponent·enabled·
// relativeTransform)를 이 모듈의 생성 코드가 import descriptor로 담는다 — 다른 모듈 base의 상속
// ObjectRefArray Trace, Level 그래프 저장·로드, 경로 쓰기를 시험한다. EngineTests만 의존한다.

// AS 6.3절 AssetLease 자리다: 이 형질을 선언한 타입은 WorldOnly 클래스의 ROCKS_NATIVE 멤버로만 둘
// 수 있다(RMC2206). 저장하지 않으므로 로드한 객체는 기본값(token 0)이다.
struct LeaseSlot {
    static constexpr bool RocksRequiresWorldOnlyOwner = true;

    uint64 token = 0;
};

// 영속 상태를 더한 Actor 파생이다(WorldOnly는 Actor에서 물려받는다). 상속 필드에 int32·String·
// SoftObjectRef<> 필드를 더하고, WorldOnly 소유자 형질 NATIVE 멤버를 둔다.
ROCKS_CLASS()
class SaveActor final : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit SaveActor(Allocator& allocator) noexcept
        : Actor(allocator), label(allocator), asset(allocator) {
    }

    ROCKS_PROPERTY() int32 score = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() SoftObjectRef<> asset;
    ROCKS_NATIVE() LeaseSlot lease;
};

// 영속 상태를 더한 SceneComponent 파생이다. 같은 Actor의 형제 Component를 가리키는 강한 참조(새
// 생존 경로를 만들지 않는다)와 값 배열을 더한다.
ROCKS_CLASS()
class SaveComponent final : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    explicit SaveComponent(Allocator& allocator) noexcept
        : SceneComponent(allocator), values(allocator) {
    }

    ROCKS_PROPERTY() ObjectRef<SaveComponent> sibling;
    ROCKS_PROPERTY() Array<int32> values;
};

} // namespace rocks::world_fixture
