#include <Core/Module/ModuleEntry.h>

namespace rocks::audio {

namespace {

// AudioOpus 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절,
// Docs/Design/AudioSystem 3.30.2절). 디코더 factory와 디코더 vtable이 이 모듈 안에 있으므로,
// 표에 등록된 factory나 만든 디코더가 남은 채 내려가지 않게 실행 중 해제를 거부한다.
class AudioOpusModule final : public ModuleImplementation {
public:
    bool SupportsUnload() const noexcept override {
        return false;
    }
};

} // namespace

} // namespace rocks::audio

ROCKS_IMPLEMENT_MODULE(rocks::audio::AudioOpusModule, AudioOpus)
