#include <ReflectionTestFixtures/SubobjectFixture.generated.h>
#include <Object/SubobjectDefaultsContext.h>

// 선언 subobject fixture의 기본값 덮기다(Docs/Design/Prefab 10.2절). base는 자기 subobject를,
// 파생은 상속 subobject(arm)와 자기 subobject(hand)를 덮는다 — 런타임이 base 것을 먼저 부르므로
// arm은 파생 값이 남는다.
namespace rocks::reflection_fixture {

namespace {

// subobject의 count를 value로 두는 덮기다.
auto SetCount(int32 value) noexcept {
    return [value](SubobjectPart& part) noexcept {
        part.count = value;
        return Result<void>::Success();
    };
}

} // namespace

Result<void> SubobjectOwner::InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept {
    if (auto body = context.With(&SubobjectOwner::body, SetCount(1)); !body) {
        return body;
    }

    return context.With(&SubobjectOwner::arm, SetCount(2));
}

Result<void>
SubobjectOwnerChild::InitSubobjectDefaults(SubobjectDefaultsContext& context) noexcept {
    if (auto arm = context.With(&SubobjectOwnerChild::arm, SetCount(20)); !arm) {
        return arm;
    }

    return context.With(&SubobjectOwnerChild::hand_, SetCount(3));
}

} // namespace rocks::reflection_fixture
