#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>

// Fixture의 생성자다. 모듈 타입 등록 함수(모듈 생성 헤더에 선언)를 부르므로 번역 단위에 둔다 —
// 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다(Docs/Design/ReflectionGeneratedSources.ko.md
// 8절).
namespace rocks::test::assign {

Fixture::Fixture(Allocator& objectAllocator, bool observed_, Allocator* observerAllocator) noexcept
    : observed(observed_), observerMemory(observerAllocator ? *observerAllocator : system),
      types(system), observer(observerMemory, ReferenceObserver::DefaultMaxKeys),
      registration(types, system),
      objects(objectAllocator, types, {0x4153, 1}, AssignOptions(observer, observed_)),
      access(objects.OwnerAccess()), diagnostics(system), roots(system) {
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    auto acquired = types.Acquire(GeneratedType<AssignHolder>::Schema().id);
    REQUIRE(acquired);
    holder = Move(acquired.Value());
    if (observed) {
        auto created = MakeTarget();
        package = created.Handle();
        REQUIRE(roots.TryPushBack(Move(created)));
    }
}

} // namespace rocks::test::assign
