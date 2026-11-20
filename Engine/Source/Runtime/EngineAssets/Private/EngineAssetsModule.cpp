#include <Core/Module/ModuleEntry.h>
#include <EngineAssets/EngineAssetsSubsystem.h>

namespace rocks {

namespace {

// EngineAssets 모듈의 수명 객체다(Docs/Design/ModuleLoader.ko.md 3.1·3.10절). 시작할 일은 없고,
// `asset.*` CVar는 HostSettings가 설정 파일을 읽기 전에 모든 모듈의 RegisterSettings를 부를 때
// 등록한다(Docs/Design/AssetSystem.ko.md 11.8절 3).
class EngineAssetsModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return RegisterEngineAssetsSettings(context);
    }
};

} // namespace

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::EngineAssetsModule, EngineAssets)
