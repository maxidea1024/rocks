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

// focus 첫 갱신(3.42.5절, UE au.FocusData.InitializeFocusFactorOnFirstUpdate —
// ActiveSound.cpp:48–53).
constinit cvar::CVar<int32> FocusInitializeOnFirstUpdateVar =
    cvar::Var("audio.focus.initializeonfirstupdate", 1)
        .Help(
            "1 sets the listener focus factor straight to its target on a sound's first update; 0 "
            "interpolates from out of focus. Read every frame.")
        .Range(0, 1);

// 차폐(3.42.7절, UE
// DefaultOcclusionCheckInterval·au.DisableOcclusion·au.EnableOcclusionFilterScale).
constinit cvar::CVar<float> OcclusionCheckIntervalVar =
    cvar::Var("audio.occlusion.checkinterval", OcclusionSettings{}.checkIntervalSeconds)
        .Help("Seconds between occlusion queries of a playing sound; below 0 queries only when the "
              "sound starts. Read every frame.")
        .Range(MinOcclusionCheckSeconds, MaxOcclusionCheckSeconds);

constinit cvar::CVar<int32> OcclusionDisableVar =
    cvar::Var("audio.occlusion.disable", 0)
        .Help("1 skips occlusion queries and treats every sound as unoccluded. Read every frame.")
        .Range(0, 1);

constinit cvar::CVar<int32> OcclusionFilterScaleVar =
    cvar::Var("audio.occlusion.filterscale", 0)
        .Help("1 scales the occlusion low-pass frequency by 0.25. Read every frame.")
        .Range(0, 1);

// 가상화(3.41.3절, UE au.VirtualLoops.* — AudioVirtualLoop.cpp:9–42, au.MaxWorldDistance).
constinit cvar::CVar<int32> VirtualEnabledVar =
    cvar::Var("audio.virtual.enabled", 1)
        .Help(
            "1 lets inaudible looping sounds become virtual (no voice, cursor keeps moving). Read "
            "every frame.")
        .Range(0, 1);

constinit cvar::CVar<int32> VirtualAllowPlayWhenSilentVar =
    cvar::Var("audio.virtual.allowplaywhensilent", 1)
        .Help("1 honors the PlayWhenSilent virtualization mode; 0 treats it as Restart. Read every "
              "frame.")
        .Range(0, 1);

constinit cvar::CVar<float> VirtualUpdateMinVar =
    cvar::Var("audio.virtual.updatemin", DefaultVirtualUpdateMinSeconds)
        .Help(
            "Shortest interval in seconds between audibility checks of a virtual sound. Read every "
            "frame.")
        .Range(0.0f, MaxVirtualUpdateSeconds);

constinit cvar::CVar<float> VirtualUpdateMaxVar =
    cvar::Var("audio.virtual.updatemax", DefaultVirtualUpdateMaxSeconds)
        .Help(
            "Longest interval in seconds between audibility checks of a virtual sound. Read every "
            "frame.")
        .Range(0.0f, MaxVirtualUpdateSeconds);

constinit cvar::CVar<float> VirtualPerfDistanceVar =
    cvar::Var("audio.virtual.perfdistance", DefaultVirtualPerfDistance)
        .Help(
            "Distance in centimeters beyond the audible range over which the check interval grows "
            "from updatemin to updatemax. Read every frame.")
        .Range(1.0f, MaxAudioWorldDistance);

constinit cvar::CVar<float> VirtualForceUpdateDistanceVar =
    cvar::Var("audio.virtual.forceupdatedistance", DefaultVirtualForceUpdateDistance)
        .Help("Listener movement in centimeters between frames that checks every virtual sound at "
              "once. Read every frame.")
        .Range(0.0f, MaxAudioWorldDistance);

constinit cvar::CVar<float> VirtualMaxWorldDistanceVar =
    cvar::Var("audio.virtual.maxworlddistance", MaxAudioWorldDistance)
        .Help("Sounds whose audible range reaches this distance in centimeters are always audible. "
              "Read every frame.")
        .Range(1.0f, MaxAudioWorldDistance);

VirtualizationSettings VirtualizationFromSettings() noexcept {
    VirtualizationSettings settings;
    settings.enabled = VirtualEnabledVar.Get() != 0;
    settings.allowPlayWhenSilent = VirtualAllowPlayWhenSilentVar.Get() != 0;
    settings.updateMinSeconds = VirtualUpdateMinVar.Get();
    settings.updateMaxSeconds = VirtualUpdateMaxVar.Get();
    settings.perfDistance = VirtualPerfDistanceVar.Get();
    settings.forceUpdateDistance = VirtualForceUpdateDistanceVar.Get();
    settings.maxWorldDistance = VirtualMaxWorldDistanceVar.Get();
    return settings;
}

} // namespace engine_audio_detail

Result<void> RegisterEngineAudioSettings(config::ConfigContext& context) noexcept {
    using namespace engine_audio_detail;
    return context.Register(
        MasterVolumeVar, UnfocusedVolumeVar, MixerBlockFramesVar, MixerBufferCountVar,
        MixerSampleRateVar, MaxVoicesVar, StreamRingFramesVar, StreamMaxStreamsVar,
        StreamDefaultLoadingVar, ConcurrencyMinDistanceDeltaVar, FocusInitializeOnFirstUpdateVar,
        OcclusionCheckIntervalVar, OcclusionDisableVar, OcclusionFilterScaleVar, VirtualEnabledVar,
        VirtualAllowPlayWhenSilentVar, VirtualUpdateMinVar, VirtualUpdateMaxVar,
        VirtualPerfDistanceVar, VirtualForceUpdateDistanceVar, VirtualMaxWorldDistanceVar);
}

} // namespace rocks
