#include "ValueRange.h"

#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioComponent.generated.h>
#include <EngineAudio/AudioWorldSubsystem.h>

// AudioComponent 수명과 재생이다(Docs/Design/AudioSystem 3.35.8절).
namespace rocks {

AudioComponent::AudioComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
}

AudioWorldSubsystem* AudioComponent::Subsystem() const noexcept {
    return HasWorld() ? GetWorld().GetSubsystem<AudioWorldSubsystem>() : nullptr;
}

Result<void> AudioComponent::Play(uint32 startFrame) noexcept {
    if (!sound_.IsValid() ||
        !engine_audio_detail::IsFiniteInRange(volumeMultiplier, 0.0f, audio::MaxVoiceGain) ||
        !engine_audio_detail::IsFiniteInRange(pitchMultiplier, audio::MinPitch, audio::MaxPitch)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AudioWorldSubsystem* audio = Subsystem();
    if (audio == nullptr || !HasSystemState(SystemStateKind::Audio)) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    Stop();
    PlayOptions options;
    options.volume = volumeMultiplier;
    options.pitch = pitchMultiplier;
    options.startFrame = startFrame;
    // emitter를 잡지 못했으면(scene이 끝남·표 가득) 2D가 아니라 위치 재생으로 낸다.
    auto played = emitter_.IsValid() ? audio->PlayAttached(emitter_, sound_, Move(options))
                                     : audio->PlayAt(sound_, WorldLocation(), Move(options));
    if (!played) {
        return Result<void>::Failure(played.Error());
    }

    playback_ = played.Value();
    return Result<void>::Success();
}

void AudioComponent::Stop(uint32 fadeFrames) noexcept {
    if (!playback_.IsValid()) {
        return;
    }

    if (AudioWorldSubsystem* audio = Subsystem()) {
        (void)audio->Stop(playback_, fadeFrames);
    }

    playback_ = PlaybackHandle{};
}

bool AudioComponent::IsPlaying() const noexcept {
    if (!playback_.IsValid()) {
        return false;
    }

    const AudioWorldSubsystem* audio = Subsystem();
    if (audio == nullptr) {
        return false;
    }

    const PlaybackStatus status = audio->Status(playback_);
    return status == PlaybackStatus::Pending || status == PlaybackStatus::Playing;
}

Result<void> AudioComponent::SetVolumeMultiplier(float value) noexcept {
    if (!engine_audio_detail::IsFiniteInRange(value, 0.0f, audio::MaxVoiceGain)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    volumeMultiplier = value;
    if (AudioWorldSubsystem* audio = Subsystem(); audio != nullptr && playback_.IsValid()) {
        return audio->SetParameter(playback_, PlaybackParameter::Volume, value);
    }

    return Result<void>::Success();
}

Result<void> AudioComponent::SetPitchMultiplier(float value) noexcept {
    if (!engine_audio_detail::IsFiniteInRange(value, audio::MinPitch, audio::MaxPitch)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    pitchMultiplier = value;
    if (AudioWorldSubsystem* audio = Subsystem(); audio != nullptr && playback_.IsValid()) {
        return audio->SetParameter(playback_, PlaybackParameter::Pitch, value);
    }

    return Result<void>::Success();
}

void AudioComponent::OnStart() noexcept {
    if (autoActivate && sound_.IsValid()) {
        (void)Play();
    }
}

void AudioComponent::OnDisable() noexcept {
    if (stopOnDisable) {
        Stop();
    }
}

bool AudioComponent::WantsSystemState(SystemStateKind kind) const noexcept {
    return kind == SystemStateKind::Audio;
}

Result<void> AudioComponent::CreateSystemState(SystemStateKind kind) noexcept {
    if (kind != SystemStateKind::Audio) {
        return Result<void>::Success();
    }

    // World에 오디오가 없거나 scene이 끝났으면 emitter 없이 등록한다(재생은 PlayAt·Ignored).
    if (AudioWorldSubsystem* audio = Subsystem()) {
        auto registered = audio->RegisterEmitter(WorldLocation());
        emitter_ = registered ? registered.Value() : AudioEmitterId{};
    }

    return Result<void>::Success();
}

void AudioComponent::DestroySystemState(SystemStateKind kind) noexcept {
    if (kind != SystemStateKind::Audio) {
        return;
    }

    // 최소 fade로 멈춘다(fade 0은 click — 스냅숏이 PCM을 쥐므로 fade 동안 수명 문제가 없다).
    // emitter를 놓으면 붙은 재생도 끝난다.
    Stop();
    if (AudioWorldSubsystem* audio = Subsystem(); audio != nullptr && emitter_.IsValid()) {
        audio->UnregisterEmitter(emitter_);
    }

    emitter_ = AudioEmitterId{};
}

void AudioComponent::OnTransformUpdated() noexcept {
    if (!emitter_.IsValid()) {
        return;
    }

    if (AudioWorldSubsystem* audio = Subsystem()) {
        audio->UpdateEmitter(emitter_, WorldLocation());
    }
}

} // namespace rocks
