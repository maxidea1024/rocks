#include <Audio/AudioDecoder.h>
#include <AudioOpus/OpusAudioDecoder.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>

namespace rocks::audio {

namespace {

// AudioOpus 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절,
// Docs/Design/AudioSystem 3.30.2·3.37.1절). 호스트가 디코더 등록부(AudioDecoderRegistry)를 올렸으면
// 시작 때 Opus factory를 넣고 종료 때 지운다. 디코더 factory와 디코더 vtable이 이 모듈 안에
// 있으므로, 표에 등록된 factory나 만든 디코더가 남은 채 내려가지 않게 실행 중 해제를 거부한다.
class AudioOpusModule final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        registry_ = context.Services().Find<AudioDecoderRegistry>();
        if (registry_ != nullptr) {
            registry_->SetFactory(AudioCodecId::Opus, &CreateOpusAudioDecoder);
        }

        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        if (registry_ != nullptr) {
            registry_->SetFactory(AudioCodecId::Opus, nullptr);
            registry_ = nullptr;
        }
    }

    bool SupportsUnload() const noexcept override {
        return false;
    }

private:
    AudioDecoderRegistry* registry_ = nullptr;
};

} // namespace

} // namespace rocks::audio

ROCKS_IMPLEMENT_MODULE(rocks::audio::AudioOpusModule, AudioOpus)
