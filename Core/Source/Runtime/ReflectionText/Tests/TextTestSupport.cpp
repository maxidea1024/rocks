#include "TextTestSupport.h"

#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>

// TextSetup의 생성자다. 모듈 타입 등록 함수(모듈 생성 헤더에 선언)를 부르므로 번역 단위에 둔다 —
// 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다(Docs/Design/ReflectionGeneratedSources.ko.md
// 8절).
namespace rocks::test::text {

TextSetup::TextSetup(Allocator& allocator) noexcept
    : types(allocator), registration(types, allocator), names(allocator), diagnostics(allocator) {
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    REQUIRE(names.Initialize());
    auto acquired = types.Acquire(GeneratedType<TextHolder>::Schema().id);
    REQUIRE(acquired);
    holder = Move(acquired.Value());
    options.names = &names;
}

} // namespace rocks::test::text
