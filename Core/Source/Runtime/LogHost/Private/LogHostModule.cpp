#include <Core/Module/ModuleEntry.h>
#include <LogHost/LogHost.h>

namespace rocks {

namespace {

// LogHost 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.1·3.10절). 시작할 일은 없고, 로그
// 설정(log.* CVar·설정 struct)은 HostSettings가 설정 파일을 읽기 전에 모든 모듈의
// RegisterSettings를 부를 때 등록한다.
class LogHostModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return RegisterLogSettings(context);
    }
};

} // namespace

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::LogHostModule, LogHost)
