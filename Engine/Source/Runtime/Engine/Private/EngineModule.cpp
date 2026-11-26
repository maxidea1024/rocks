#include <Core/Module/ModuleEntry.h>
#include <Engine/SplashSettings.h>

// Engine 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일은 없고, HostSettings가 설정
// 파일을 읽기 전에 모든 모듈의 RegisterSettings를 부를 때 `ui.splash.*`를 등록한다
// (Docs/Design/SplashScreen 10절).
namespace rocks {

namespace {

class EngineModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return RegisterSplashSettings(context);
    }
};

} // namespace

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::EngineModule, Engine)
