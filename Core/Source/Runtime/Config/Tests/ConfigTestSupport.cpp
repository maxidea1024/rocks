#include "ConfigTestSupport.h"

#include <Config/Config.module.generated.h>
#include <ConfigTestFixtures/ConfigTestFixtures.module.generated.h>

// ConfigFixture의 생성자다. 모듈 타입 등록 함수(모듈 생성 헤더에 선언)를 부르므로 번역 단위에 둔다
// — 손으로 쓴 헤더는 모듈 생성 헤더를 include하지
// 않는다(Docs/Design/ReflectionGeneratedSources.ko.md 8절).
namespace rocks::test::config {

ConfigFixture::ConfigFixture(const ConfigContextOptions& options) noexcept
    : types(system), registration(types, system), names(system),
      objects(system, types, {0x434f4e46, 1}, MakeOptions(names)) {
    REQUIRE(generated::Config::RegisterModuleTypes(registration));
    REQUIRE(generated::ConfigTestFixtures::RegisterModuleTypes(registration));
    REQUIRE(names.Initialize());
    auto made = UniquePtr<ConfigContext>::TryMake(system, system, objects, options);
    REQUIRE(made);
    context = Move(made.Value());
    REQUIRE(context->Initialize());
    REQUIRE(context->RegisterSettings<GraphSettings>());
    REQUIRE(context->RegisterSettings<GameplaySettings>());
    REQUIRE(context->Sync());
}

} // namespace rocks::test::config
