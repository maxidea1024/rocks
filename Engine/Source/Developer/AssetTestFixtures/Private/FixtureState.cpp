#include <AssetTestFixtures/TestAssets.h>

// 시험 fixture의 hook 저장소다. 공개 헤더에 쓰기 가능한 정적 저장소를 두지 않으므로
// (Docs/Design/BuildTargets 3.8절 BD29) 선언은 헤더에, 정의는 여기에 둔다.
namespace rocks::asset_fixture {

TestHookAssetHooks& ActiveTestHookAssetHooks() noexcept {
    static constinit TestHookAssetHooks hooks{};
    return hooks;
}

} // namespace rocks::asset_fixture
