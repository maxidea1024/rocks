#include <Engine/Engine.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <EngineAudio/AudioWorldSubsystem.h>

ROCKS_DEFINE_NATIVE_TYPE_KEY(rocks::AudioWorldSubsystem)

namespace rocks {

AudioWorldSubsystem::AudioWorldSubsystem(Allocator& allocator) noexcept {
    (void)allocator;
}

AudioWorldSubsystem::~AudioWorldSubsystem() = default;

WorldSubsystemType AudioWorldSubsystem::Type() noexcept {
    WorldSubsystemType type;
    type.key = SubsystemKeyOf<AudioWorldSubsystem>();
    type.name = "Audio";
    type.create = [](Allocator& allocator) noexcept -> Result<UniquePtr<WorldSubsystem>> {
        auto created = UniquePtr<AudioWorldSubsystem>::TryMake(allocator, allocator);
        if (!created) {
            return Result<UniquePtr<WorldSubsystem>>::Failure(created.Error());
        }

        return Result<UniquePtr<WorldSubsystem>>::Success(
            UniquePtr<WorldSubsystem>(Move(created.Value())));
    };
    return type;
}

Result<void> AudioWorldSubsystem::Initialize(World& world) noexcept {
    engine_ = GetEngineSubsystem<AudioEngineSubsystem>(world);
    if (engine_ != nullptr) {
        scene_ = engine_->RegisterScene();
    }

    return Result<void>::Success();
}

void AudioWorldSubsystem::OnWorldEndPlay(World& world) noexcept {
    (void)world;
    ReleaseScene();
}

void AudioWorldSubsystem::Deinitialize(World& world) noexcept {
    (void)world;
    ReleaseScene();
    engine_ = nullptr;
}

void AudioWorldSubsystem::ReleaseScene() noexcept {
    if (engine_ != nullptr && scene_.IsValid()) {
        engine_->UnregisterScene(scene_);
    }

    scene_ = AudioSceneId{};
}

Result<PlaybackHandle> AudioWorldSubsystem::Play2D(const SoundSourceRef& sound,
                                                   PlayOptions options) noexcept {
    if (engine_ == nullptr) {
        return Result<PlaybackHandle>::Success(PlaybackHandle{});
    }

    return engine_->Play2DInScene(scene_, sound, Move(options));
}

Result<PlaybackHandle> AudioWorldSubsystem::PlayAt(const SoundSourceRef& sound,
                                                   const Vec3d& position,
                                                   PlayOptions options) noexcept {
    if (engine_ == nullptr) {
        return Result<PlaybackHandle>::Success(PlaybackHandle{});
    }

    return engine_->PlayAtInScene(scene_, sound, position, Move(options));
}

Result<PlaybackHandle> AudioWorldSubsystem::PlayAttached(AudioEmitterId emitter,
                                                         const SoundSourceRef& sound,
                                                         PlayOptions options) noexcept {
    if (engine_ == nullptr) {
        return Result<PlaybackHandle>::Success(PlaybackHandle{});
    }

    return engine_->PlayAttachedInScene(scene_, emitter, sound, Move(options));
}

Result<void> AudioWorldSubsystem::Stop(PlaybackHandle handle, uint32 fadeFrames) noexcept {
    return engine_ != nullptr ? engine_->Stop(handle, fadeFrames) : Result<void>::Success();
}

Result<void> AudioWorldSubsystem::SetParameter(PlaybackHandle handle, PlaybackParameter parameter,
                                               float value, uint32 rampFrames) noexcept {
    return engine_ != nullptr ? engine_->SetParameter(handle, parameter, value, rampFrames)
                              : Result<void>::Success();
}

PlaybackStatus AudioWorldSubsystem::Status(PlaybackHandle handle) const noexcept {
    return engine_ != nullptr ? engine_->Status(handle) : PlaybackStatus::Finished;
}

void AudioWorldSubsystem::SetListener(const Vec3d& position, const Quatd& rotation) noexcept {
    if (engine_ != nullptr) {
        engine_->SetSceneListener(scene_, position, rotation);
    }
}

Result<AudioEmitterId> AudioWorldSubsystem::RegisterEmitter(const Vec3d& position) noexcept {
    if (engine_ == nullptr) {
        return Result<AudioEmitterId>::Failure(ErrorCode::InvalidArgument);
    }

    return engine_->RegisterEmitter(scene_, position);
}

void AudioWorldSubsystem::UpdateEmitter(AudioEmitterId emitter, const Vec3d& position) noexcept {
    if (engine_ != nullptr) {
        engine_->UpdateEmitter(emitter, position);
    }
}

void AudioWorldSubsystem::UnregisterEmitter(AudioEmitterId emitter) noexcept {
    if (engine_ != nullptr) {
        engine_->UnregisterEmitter(emitter);
    }
}

} // namespace rocks
