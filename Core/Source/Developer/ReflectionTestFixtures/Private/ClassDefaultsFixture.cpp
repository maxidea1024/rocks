#include <ReflectionTestFixtures/ClassDefaultsFixture.generated.h>

// non-null ClassRef 기본값 fixture의 생성자다(ClassRef::Of는 대상의 생성 헤더가 보여야 한다 — 공개
// 헤더는 자기 생성 헤더를 include할 수 없다).
namespace rocks::reflection_fixture {

DefaultsKindPick::DefaultsKindPick() noexcept
    : pick(ClassRef<DefaultsAbstractBase>::Of<DefaultsHolder>()) {
}

} // namespace rocks::reflection_fixture
