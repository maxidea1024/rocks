#include <AudioMiniaudio/MiniaudioOutput.h>

#include <Core/Diagnostics/Log.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>

namespace rocks::audio {

namespace {

constexpr LogCategory LogAudioMiniaudio{"AudioMiniaudio"};

// AudioMiniaudio 모듈의 수명 객체다(Docs/Design/AudioSystem 3.34.4절,
// Docs/Design/ModuleLoader 3.12절). 호스트가 AudioOutputBackendRegistry를 서비스로 올렸으면
// miniaudio factory를 넣고 Shutdown에서 자기 것일 때만 뺀다. 서비스가 없으면(장치를 쓰지 않는
// 호스트) 아무 일도 하지 않는다. backend vtable과 장치 콜백이 이 모듈 코드라 실행 중 해제를
// 거부한다.
class AudioMiniaudioModule final : public ModuleImplementation {
public:
    Result<void> Startup(ModuleStartContext& context) noexcept override {
        AudioOutputBackendRegistry* registry =
            context.Services().Find<AudioOutputBackendRegistry>();
        if (registry == nullptr) {
            return Result<void>::Success();
        }

        if (!RegisterMiniaudioOutput(*registry)) {
            Log::Info(
                LogAudioMiniaudio,
                "An audio output backend is already registered; the miniaudio backend stays idle");
            return Result<void>::Success();
        }

        registry_ = registry;
        return Result<void>::Success();
    }

    void Shutdown() noexcept override {
        if (registry_ != nullptr) {
            UnregisterMiniaudioOutput(*registry_);
            registry_ = nullptr;
        }
    }

    bool SupportsUnload() const noexcept override {
        return false;
    }

private:
    AudioOutputBackendRegistry* registry_ = nullptr;
};

} // namespace

} // namespace rocks::audio

ROCKS_IMPLEMENT_MODULE(rocks::audio::AudioMiniaudioModule, AudioMiniaudio)
