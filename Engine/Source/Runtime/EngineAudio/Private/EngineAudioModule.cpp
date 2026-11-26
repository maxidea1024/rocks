#include <Core/Module/ModuleEntry.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <EngineAudio/EngineAudio.module.generated.h>
#include <EngineAudio/EngineAudioTypes.h>

// EngineAudio 모듈 수명 객체와 설정·타입 등록이다(Docs/Design/AudioSystem 3.35.1절). 리플렉션
// 타입은 엔진의 모듈 타입 등록기가 등록하고, audio.* 설정은 HostSettings가 설정 파일을 읽기 전에
// 모든 모듈의 RegisterSettings를 부를 때 등록한다.
namespace rocks {

namespace {

class EngineAudioModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return RegisterEngineAudioSettings(context);
    }
};

} // namespace

Result<void> RegisterEngineAudioTypes(ModuleRegistration& registration) noexcept {
    return generated::EngineAudio::RegisterModuleTypes(registration);
}

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::EngineAudioModule, EngineAudio)
