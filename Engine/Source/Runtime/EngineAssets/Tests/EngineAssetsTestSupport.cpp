#include "EngineAssetsTestSupport.h"

#include <AssetTestFixtures/AssetTestFixtures.module.generated.h>

// EngineAssets 시험 공용 도구의 정의다(EngineAssetsTestSupport.h).
namespace rocks::engine_assets_test {

Result<void> RegisterFixtureTypes(ModuleRegistration& modules) noexcept {
    return generated::AssetTestFixtures::RegisterModuleTypes(modules);
}

} // namespace rocks::engine_assets_test
