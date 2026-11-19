#include <Config/DeviceProfiles.h>
#include <Core/Module/ModuleEntry.h>

namespace rocks::config {

namespace {

// Config 모듈의 수명 객체다(Docs/Design/ModuleLoader.ko.md 3.1·3.10절). 시작할 일은 없고, 기기
// 프로필 설정은 HostSettings가 설정 파일을 읽기 전에 모든 모듈의 RegisterSettings를 부를 때
// 등록한다.
class ConfigModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(ConfigContext& context) noexcept {
        return RegisterDeviceProfileSettings(context);
    }
};

} // namespace

} // namespace rocks::config

ROCKS_IMPLEMENT_MODULE(rocks::config::ConfigModule, Config)
