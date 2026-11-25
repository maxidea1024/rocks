#include <Audio/AudioLog.h>
#include <Audio/AudioOutputDevice.h>

#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Platform/FloatingPointEnvironment.h>

ROCKS_DEFINE_HOST_SERVICE(rocks::audio::AudioOutputBackendRegistry)

namespace rocks::audio {

namespace {

const char* PriorityName(platform::ThreadPriority priority) noexcept {
    switch (priority) {
    case platform::ThreadPriority::Lowest:
        return "Lowest";
    case platform::ThreadPriority::BelowNormal:
        return "BelowNormal";
    case platform::ThreadPriority::Normal:
        return "Normal";
    case platform::ThreadPriority::AboveNormal:
        return "AboveNormal";
    case platform::ThreadPriority::Highest:
        return "Highest";
    }

    return "Unknown";
}

// AU2 믹서가 받는 출력 레이아웃이다(3.22.2절 — 다채널은 AU9).
bool IsSupportedOutputLayout(ChannelLayout layout) noexcept {
    return layout == ChannelLayout::Mono || layout == ChannelLayout::Stereo;
}

} // namespace

AudioOutputDevice::AudioOutputDevice(Allocator& allocator,
                                     const AudioOutputDeviceOptions& options) noexcept
    : allocator_(allocator), options_(options), null_(allocator, options.nullMode),
      callback_(ring_) {
}

AudioOutputDevice::~AudioOutputDevice() noexcept {
    Close();
}

Result<UniquePtr<AudioOutputDevice>>
AudioOutputDevice::TryOpen(const AudioMixerConfig& mixerConfig,
                           UniquePtr<AudioOutputBackend> backend, const AudioOutputRequest& request,
                           const AudioOutputDeviceOptions& options, Allocator& allocator) noexcept {
    auto made = UniquePtr<AudioOutputDevice>::TryMake(allocator, allocator, options);
    if (!made) {
        return Result<UniquePtr<AudioOutputDevice>>::Failure(made.Error());
    }

    AudioOutputDevice& device = *made.Value();
    AudioOutputRequest effective = request;
    if (effective.callbackFrames == 0) {
        effective.callbackFrames = mixerConfig.blockFrames;
    }

    device.backend_ = Move(backend);
    if (device.backend_) {
        if (auto opened = device.OpenWith(*device.backend_, mixerConfig, effective); !opened) {
            // 실 backend 실패는 Null로 진행하고 원래 오류를 남긴다(3.33.4절). 설정 오류는 Null로도
            // 같으므로 그대로 돌려준다.
            if (opened.Error() == ErrorCode::InvalidArgument) {
                return Result<UniquePtr<AudioOutputDevice>>::Failure(opened.Error());
            }

            device.info_.backendError = opened.Error();
            device.info_.backendNativeError = device.backend_->LastNativeError();
            Log::Warning(LogAudio,
                         "Audio output backend {Backend} failed ({Error}, native {Native}); using "
                         "Null output",
                         device.backend_->Name(), ErrorCodeName(opened.Error()),
                         device.info_.backendNativeError);
        }
    }

    if (device.active_ == nullptr) {
        if (auto opened = device.OpenWith(device.null_, mixerConfig, effective); !opened) {
            return Result<UniquePtr<AudioOutputDevice>>::Failure(opened.Error());
        }

        device.info_.usingNull = true;
    }

    const AudioOutputDeviceInfo& info = device.info_;
    Log::Info(LogAudio,
              "Audio output opened: {Backend}, {Rate} Hz, {Channels} channels, callback {Callback} "
              "frames, "
              "{Buffers} buffers of {Block} frames, AudioRT priority {Priority}",
              info.backendName, info.format.sampleRate, ChannelCount(info.format.layout),
              info.format.callbackFrames, info.bufferCount, device.mixer_->Config().blockFrames,
              info.priorityKnown ? PriorityName(info.priority) : "unknown");
    return Result<UniquePtr<AudioOutputDevice>>::Success(Move(made.Value()));
}

// backend 하나로 연다. 실패하면 이 시도가 만든 것을 모두 되돌린다(active_는 비운 채).
Result<void> AudioOutputDevice::OpenWith(AudioOutputBackend& backend,
                                         const AudioMixerConfig& mixerConfig,
                                         const AudioOutputRequest& request) noexcept {
    auto format = backend.Open(request, callback_);
    if (!format) {
        backend.Close();
        return Result<void>::Failure(
            format.Error() == ErrorCode::InvalidArgument ? ErrorCode::Unsupported : format.Error());
    }

    if (!IsSupportedOutputLayout(format.Value().layout) ||
        !IsValidMixSampleRate(format.Value().sampleRate) || format.Value().callbackFrames == 0) {
        backend.Close();
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 장치가 rate를 정한다(UE와 같다). 믹서 설정의 나머지는 그대로다.
    AudioMixerConfig config = mixerConfig;
    config.sampleRate = format.Value().sampleRate;
    config.layout = format.Value().layout;
    auto mixer = AudioMixer::TryCreate(config, allocator_);
    if (!mixer) {
        backend.Close();
        return Result<void>::Failure(mixer.Error());
    }

    mixer_ = Optional<AudioMixer>(Move(mixer.Value()));
    const uint32 bufferCount = ChooseOutputBufferCount(
        format.Value().callbackFrames, config.blockFrames, options_.requestedBufferCount);
    const uint32 channels = ChannelCount(config.layout);
    if (auto ring = ring_.Initialize(allocator_, bufferCount, config.blockFrames, channels,
                                     &render_.WakeSignal());
        !ring) {
        mixer_ = NullOpt;
        backend.Close();
        return ring;
    }

    if (auto render = render_.Initialize(allocator_, *mixer_, ring_); !render) {
        ring_.Shutdown();
        mixer_ = NullOpt;
        backend.Close();
        return render;
    }

    // 링을 채운 뒤 backend를 시작한다(첫 콜백이 underrun이 아니게).
    if (options_.renderMode == AudioRenderMode::Thread) {
        if (auto started = render_.Start(); !started) {
            render_.Shutdown();
            ring_.Shutdown();
            mixer_ = NullOpt;
            backend.Close();
            return started;
        }
    } else {
        RenderAvailable();
    }

    if (auto started = backend.Start(); !started) {
        render_.Stop();
        render_.Shutdown();
        ring_.Shutdown();
        mixer_ = NullOpt;
        backend.Close();
        return Result<void>::Failure(started.Error() == ErrorCode::InvalidArgument
                                         ? ErrorCode::Unsupported
                                         : started.Error());
    }

    active_ = &backend;
    started_ = true;
    info_.format = format.Value();
    info_.bufferCount = bufferCount;
    info_.backendName = backend.Name();
    info_.priorityKnown = render_.AppliedPriority(info_.priority);
    return Result<void>::Success();
}

void AudioOutputDevice::RenderAvailable() noexcept {
    // 수동 방식: 이 스레드가 AudioRT다. 실시간 FP 환경 안에서 렌더한다.
    platform::RunWithRealtimeAudioFloatingPoint(
        [](void* context) noexcept { static_cast<AudioRenderThread*>(context)->RenderAvailable(); },
        &render_);
}

void AudioOutputDevice::CloseActive() noexcept {
    if (active_ == nullptr) {
        return;
    }

    // Stop이 돌아오면 콜백이 더 불리지 않는다(3.7.1절). 그 뒤에만 AudioRT·링을 거둔다.
    (void)active_->Stop();
    render_.Stop();
    active_->Close();
    render_.Shutdown();
    ring_.Shutdown();
    mixer_ = NullOpt;
    active_ = nullptr;
    started_ = false;
}

void AudioOutputDevice::Close() noexcept {
    if (active_ != nullptr) {
        Log::Info(LogAudio, "Audio output closed: {Backend}", info_.backendName);
    }

    CloseActive();
    backend_.Reset();
}

} // namespace rocks::audio
