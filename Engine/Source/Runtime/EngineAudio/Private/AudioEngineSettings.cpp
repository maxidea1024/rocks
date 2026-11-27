#include "AudioEngineSettingsInternal.h"

#include <Audio/AudioMixerTypes.h>
#include <Audio/AudioOutputRing.h>
#include <Audio/AudioStreamRing.h>
#include <Audio/AudioStreamService.h>
#include <AudioAssets/SoundClip.h>
#include <Config/CVar.h>
#include <Config/ConfigContext.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <EngineAudio/AudioSpatialization.h>

// `audio.*` CVar 정의다(3.35.10절). 부팅 값은 AudioEngineSubsystem::Initialize가 한 번 읽고, 볼륨은
// 프레임 끝(OnFrameEnd)에 설정 sync 뒤 값을 읽는다.
namespace rocks {

namespace engine_audio_detail {

constinit cvar::CVar<float> MasterVolumeVar =
    cvar::Var("audio.master.volume", 1.0f)
        .Help("Master output volume (linear). Applied at the next frame end.")
        .Range(0.0f, audio::MaxMasterGain);

constinit cvar::CVar<float> UnfocusedVolumeVar =
    cvar::Var("audio.unfocused.volume", 0.0f)
        .Help("Output volume multiplier while the application window is not in the foreground.")
        .Range(0.0f, 1.0f);

constinit cvar::BootCVar<int32> MixerBlockFramesVar =
    cvar::Boot("audio.mixer.blockframes", int32{audio::DefaultMixBlockFrames})
        .Help("Mixer block size in frames: 64, 128, 256, 512 or 1024. Fixed after startup.")
        .Range(int32{audio::MinMixBlockFrames}, int32{audio::MaxMixBlockFrames});

constinit cvar::BootCVar<int32> MixerBufferCountVar =
    cvar::Boot("audio.mixer.buffercount", 0)
        .Help("Output ring buffer count. 0 uses the computed count (at least 2). Fixed after "
              "startup.")
        .Range(0, int32{audio::MaxOutputBufferCount});

constinit cvar::BootCVar<int32> MixerSampleRateVar =
    cvar::Boot("audio.mixer.samplerate", 0)
        .Help("Requested mixer sample rate in Hz. 0 uses the device default. Fixed after startup.")
        .Range(0, int32{MaxMixerSampleRateSetting});

constinit cvar::BootCVar<int32> MaxVoicesVar =
    cvar::Boot("audio.voices.max", int32{DefaultMaxActiveVoices})
        .Help("Maximum number of real voices playing at once. New plays beyond it end with "
              "VoiceLimit. Fixed after startup.")
        .Range(1, int32{MaxActiveVoicesSetting});

constinit cvar::BootCVar<int32> StreamRingFramesVar =
    cvar::Boot("audio.stream.ringframes", int32{audio::StreamRingMinFrames})
        .Help("PCM ring size in frames for each streaming voice (Docs/Design/AudioSystem 3.40.3). "
              "Fixed after startup.")
        .Range(int32{audio::StreamRingMinFrames}, int32{audio::StreamRingMaxFrames});

static_assert(MixerVoiceHeadroomFactor == 2,
              "audio.stream.maxstreams help text says twice audio.voices.max");

constinit cvar::BootCVar<int32> StreamMaxStreamsVar =
    cvar::Boot("audio.stream.maxstreams", 0)
        .Help("Number of streaming decode slots. 0 uses twice audio.voices.max. Fixed after "
              "startup.")
        .Range(0, int32{audio::MaxStreamSlots});

constinit cvar::CVar<int32> StreamDefaultLoadingVar =
    cvar::Var("audio.stream.defaultloading",
              int32{static_cast<uint8>(SoundLoadingBehavior::LoadOnDemand)})
        .Help("Loading behavior used by sound clips set to Inherited: 1 RetainOnLoad, 2 "
              "PrimeOnLoad, 3 LoadOnDemand. Read when a clip is loaded.")
        .Range(int32{static_cast<uint8>(SoundLoadingBehavior::RetainOnLoad)},
               int32{static_cast<uint8>(SoundLoadingBehavior::LoadOnDemand)});

constinit cvar::CVar<float> ConcurrencyMinDistanceDeltaVar =
    cvar::Var("audio.concurrency.mindistancedelta", 0.0f)
        .Help("Distance in centimeters within which StopFarthest concurrency rules treat sounds as "
              "equally far. Read every frame.")
        .Range(0.0f, MaxAudioWorldDistance);

} // namespace engine_audio_detail

Result<void> RegisterEngineAudioSettings(config::ConfigContext& context) noexcept {
    using namespace engine_audio_detail;
    return context.Register(MasterVolumeVar, UnfocusedVolumeVar, MixerBlockFramesVar,
                            MixerBufferCountVar, MixerSampleRateVar, MaxVoicesVar,
                            StreamRingFramesVar, StreamMaxStreamsVar, StreamDefaultLoadingVar,
                            ConcurrencyMinDistanceDeltaVar);
}

} // namespace rocks
