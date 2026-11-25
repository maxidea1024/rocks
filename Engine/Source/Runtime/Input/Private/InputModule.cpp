#include <Core/Module/ModuleEntry.h>
#include <Input/InputSettings.h>

namespace rocks {

namespace {

// Input 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.1·3.10절). 시작할 일은 없고, input.* 설정은
// HostSettings가 설정 파일을 읽기 전에 모든 모듈의 RegisterSettings를 부를 때 등록한다.
class InputModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return input::RegisterInputSettings(context);
    }
};

} // namespace

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::InputModule, Input)
