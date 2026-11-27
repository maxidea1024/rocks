#include <ReflectionTestFixtures/SubobjectFixture.generated.h>

// 선언 subobject fixture의 기본값 덮기다(Docs/Design/Prefab 10.2절). 값을 주는 문맥
// (SubobjectDefaultsContext)은 PF1b에서 생긴다 — 지금은 등록 표 시험만 쓰므로 아무것도 덮지 않는다.
namespace rocks::reflection_fixture {

Result<void> SubobjectOwner::InitSubobjectDefaults(SubobjectDefaultsContext&) noexcept {
    return Result<void>::Success();
}

Result<void> SubobjectOwnerChild::InitSubobjectDefaults(SubobjectDefaultsContext&) noexcept {
    return Result<void>::Success();
}

} // namespace rocks::reflection_fixture
