#include <AudioAssets/AudioAssets.module.generated.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <Core/Module/ModuleEntry.h>

// AudioAssets 모듈의 진입점이다(Docs/Design/AudioSystem 3.36.1절). 시작할 일이 없다 — 리플렉션
// 타입은 엔진·에셋 명령 세션의 모듈 타입 등록기가 등록한다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, AudioAssets)

namespace rocks {

Result<void> RegisterAudioAssetsTypes(ModuleRegistration& registration) noexcept {
    return generated::AudioAssets::RegisterModuleTypes(registration);
}

} // namespace rocks
