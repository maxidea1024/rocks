#include <Core/Module/ModuleEntry.h>
#include <TraceControl/TraceController.h>

namespace rocks::trace_control {

namespace {

// TraceControl 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.1·3.10절). 시작할 일은 없고,
// trace 설정(trace.* CVar·프리셋 설정 struct·콘솔 영역)은 HostSettings가 설정 파일을 읽기 전에 모든
// 모듈의 RegisterSettings를 부를 때 등록한다.
class TraceControlModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return RegisterTraceSettings(context);
    }
};

} // namespace

} // namespace rocks::trace_control

ROCKS_IMPLEMENT_MODULE(rocks::trace_control::TraceControlModule, TraceControl)
