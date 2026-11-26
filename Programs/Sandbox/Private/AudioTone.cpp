#include "AudioTone.h"

#include <Audio/AudioOutputDevice.h>
#include <Audio/Decibels.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Core/Move.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Platform/Thread.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::audio;

inline constexpr LogCategory LogSandboxAudio{"Sandbox.Audio"};

constexpr float ToneHz = 440.0f;
constexpr float ToneDecibels = -12.0f;
constexpr uint32 MillisecondsPerSecond = 1000;
// 시험음 콜백 크기 요청이다(믹서 블록과 같게 — 장치가 다른 값을 고르면 링이 흡수한다).
constexpr uint32 ToneCallbackFrames = DefaultMixBlockFrames;

int Fail(const char* what, ErrorCode error) noexcept {
    Log::Error(LogSandboxAudio, "Audio tone: {What} failed: {Error}", what, ErrorCodeName(error));
    return 1;
}

// 장치를 열고 1초 사인 clip(정수 주기라 이음매 없이 반복)을 loop로 재생한 뒤 seconds초 기다린다.
int PlayTone(Allocator& allocator, AudioOutputBackendRegistry& registry, uint32 seconds) noexcept {
    UniquePtr<AudioOutputBackend> backend;
    if (AudioOutputBackendFactory factory = registry.Factory()) {
        auto made = factory(allocator);
        if (!made) {
            return Fail("backend creation", made.Error());
        }

        backend = Move(made.Value());
    } else {
        Log::Warning(LogSandboxAudio, "No audio output backend is registered; using Null output");
    }

    AudioOutputRequest request;
    request.callbackFrames = ToneCallbackFrames;
    auto opened = AudioOutputDevice::TryOpen(AudioMixerConfig{}, Move(backend), request,
                                             AudioOutputDeviceOptions{}, allocator);
    if (!opened) {
        return Fail("device open", opened.Error());
    }

    AudioOutputDevice& device = *opened.Value();
    AudioMixer& mixer = device.Mixer();
    const uint32 rate = mixer.Config().sampleRate;
    Array<float> samples(allocator);
    if (!samples.TryResize(rate)) {
        return Fail("tone allocation", ErrorCode::OutOfMemory);
    }

    const float gain = DecibelsToLinear(ToneDecibels);
    for (uint32 frame = 0; frame < rate; ++frame) {
        samples[frame] = gain * math::Sin(math::TwoPi<float> * ToneHz * static_cast<float>(frame) /
                                          static_cast<float>(rate));
    }

    AudioClipDesc desc;
    desc.channelCount = 1;
    desc.channels[0] = samples.Data();
    desc.frameCount = rate;
    desc.sampleRate = rate;
    auto clip = mixer.RegisterClip(desc);
    auto voice = mixer.AcquireVoice();
    if (!clip || !voice) {
        return Fail("tone setup", clip ? voice.Error() : clip.Error());
    }

    auto play = mixer.PreparePlay(voice.Value(), clip.Value(), 0, true, AudioVoiceParams{});
    if (!play) {
        return Fail("tone play", play.Error());
    }

    if (auto submitted = device.Submit(play.Value()); !submitted) {
        return Fail("tone submit", submitted.Error());
    }

    const AudioOutputRingCounters before = device.RingCountersSnapshot();
    for (uint32 second = 0; second < seconds; ++second) {
        platform::SleepFor(MillisecondsPerSecond);
    }

    const AudioOutputRingCounters after = device.RingCountersSnapshot();
    const AudioRenderCounters render = device.RenderCountersSnapshot();
    const AudioOutputDeviceInfo& info = device.Info();
    const uint64 underrunFrames = after.underrunFrames - before.underrunFrames;
    Log::Info(
        LogSandboxAudio,
        "Audio tone: {Seconds} s on {Backend}, {Rate} Hz, callback {Callback} frames, {Buffers} "
        "buffers, consumed {Consumed} frames, rendered {Blocks} blocks, underrun {Underrun} frames "
        "in {Callbacks} callbacks, stalled waits {Stalls}",
        seconds, info.backendName, info.format.sampleRate, info.format.callbackFrames,
        info.bufferCount, after.consumedFrames, render.renderedBlocks, underrunFrames,
        after.underrunCallbacks - before.underrunCallbacks, render.stalledWaits);
    const bool usingNull = info.usingNull;
    device.Close();
    if (usingNull) {
        Log::Warning(LogSandboxAudio,
                     "Audio tone: no output device ({Error}, native {Native}); not judged",
                     ErrorCodeName(info.backendError), info.backendNativeError);
        return 2;
    }

    return underrunFrames == 0 ? 0 : 1;
}

} // namespace

int RunAudioTone(Allocator& allocator, ModuleLoader& modules, uint32 seconds) noexcept {
    AudioOutputBackendRegistry registry;
    if (auto registered = modules.Services().Register(registry); !registered) {
        return Fail("backend registry registration", registered.Error());
    }

    const ModuleLoadReport started = modules.StartEngineGroup();
    int result = 1;
    if (started.IsOk()) {
        result = PlayTone(allocator, registry, seconds);
        modules.ShutdownEngineGroup();
    } else {
        Log::Error(
            LogSandboxAudio, "Engine modules could not start: {Module} at {Stage}: {Message}",
            StringView(started.failed), StringView(started.stage), StringView(started.message));
    }

    (void)modules.Services().Unregister(registry);
    return result;
}

} // namespace sandbox
