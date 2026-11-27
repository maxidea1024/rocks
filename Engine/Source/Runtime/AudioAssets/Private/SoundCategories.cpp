#include "AudioAssetsValidation.h"

#include <AudioAssets/AudioObjectKey.h>
#include <AudioAssets/SoundSource.generated.h>
#include <AudioAssets/MixSnapshot.generated.h>
#include <AudioAssets/SoundCategory.generated.h>
#include <AudioAssets/SoundCategoryPassiveMix.generated.h>
#include <AudioAssets/MixCategoryAdjuster.generated.h>
#include <Audio/AudioMixerTypes.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <Object/ObjectRegistry.h>

// 분류·믹스 스냅숏 에셋의 스냅숏 만들기와 값 검사다(Docs/Design/AudioSystem 3.43.1절).
namespace rocks {

namespace {

inline constexpr LogCategory LogAudioCategories{"AudioCategories"};

// 필터 주파수 상한(Hz — UE MAX_FILTER_FREQUENCY)이다.
constexpr float MaxCategoryFilterFrequency = MaxAttenuationFilterFrequency;

bool NonNegativeFinite(float value) noexcept {
    return audio_assets_detail::IsNonNegativeFinite(value);
}

bool FiniteWithin(float value, float low, float high) noexcept {
    return audio_assets_detail::IsFiniteWithin(value, low, high);
}

Result<void> ValidateProperties(const SoundCategoryProperties& properties) noexcept {
    if (!NonNegativeFinite(properties.volume) || !math::IsFinite(properties.pitch) ||
        !(properties.pitch > 0.0f) || properties.pitch > audio::MaxPitch ||
        !FiniteWithin(properties.lowPassFrequency, 0.0f, MaxCategoryFilterFrequency) ||
        !NonNegativeFinite(properties.attenuationDistanceScale) ||
        static_cast<uint32>(properties.userVolume) >= SoundUserVolumeCount ||
        static_cast<uint8>(properties.loadingBehavior) >
            static_cast<uint8>(SoundLoadingBehavior::ForceInline)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

SoundCategoryProperties PropertiesOf(const SoundCategory& category) noexcept {
    SoundCategoryProperties properties;
    properties.volume = category.volume;
    properties.pitch = category.pitch;
    properties.lowPassFrequency = category.lowPassFrequency;
    properties.attenuationDistanceScale = category.attenuationDistanceScale;
    properties.alwaysPlay = category.alwaysPlay;
    properties.isUiSound = category.isUiSound;
    properties.isMusic = category.isMusic;
    properties.userVolume = category.userVolume;
    properties.loadingBehavior = category.loadingBehavior;
    return properties;
}

// 분류 하나를 사슬 원소로 옮긴다(passive 믹스는 평탄화).
Result<void> FillLink(const SoundCategory& category, Allocator& allocator, ObjectRegistry& registry,
                      const ObjectAccessContext& access, SoundCategoryLink& link) noexcept {
    link.key = AudioObjectKey(category.Handle());
    link.properties = PropertiesOf(category);
    if (auto valid = ValidateProperties(link.properties); !valid) {
        return valid;
    }

    if (category.passiveMixes.Size() > MaxCategoryPassiveMixes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    link.passiveCount = 0;
    for (usize index = 0; index < category.passiveMixes.Size(); ++index) {
        const SoundCategoryPassiveMix& entry = category.passiveMixes[index];
        auto mix = registry.Resolve<MixSnapshot>(access, entry.mix.Handle());
        if (!mix) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto resolved = mix.Value().Get();
        if (!resolved) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto made = resolved.Value()->MakeRef(allocator, registry, access);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        SoundCategoryPassiveMixValue& value = link.passive[link.passiveCount++];
        value.mix = made.Value();
        value.minVolumeThreshold = entry.minVolumeThreshold;
        value.maxVolumeThreshold = entry.maxVolumeThreshold;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateMixSnapshotData(const MixSnapshotData& data) noexcept {
    if (data.adjusterCount > MaxMixAdjusters) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 시간은 MaxMixSnapshotSeconds 이하다(mix frame 변환이 넘치지 않게). duration은 음수가
    // 무한이다.
    if (data.key == 0 || !FiniteWithin(data.initialDelay, 0.0f, MaxMixSnapshotSeconds) ||
        !FiniteWithin(data.fadeInTime, 0.0f, MaxMixSnapshotSeconds) ||
        !math::IsFinite(data.duration) || data.duration > MaxMixSnapshotSeconds ||
        !FiniteWithin(data.fadeOutTime, 0.0f, MaxMixSnapshotSeconds)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < data.adjusterCount; ++index) {
        const MixAdjusterValue& adjuster = data.adjusters[index];
        if (adjuster.categoryKey == 0 || !NonNegativeFinite(adjuster.volume) ||
            !FiniteWithin(adjuster.pitch, 0.0f, MaxMixAdjusterPitch) ||
            !FiniteWithin(adjuster.lowPassFrequency, 0.0f, MaxCategoryFilterFrequency)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateSoundCategoryChain(const SoundCategoryChain& chain) noexcept {
    if (chain.depth > MaxSoundCategoryDepth) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (chain.depth == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < chain.depth; ++index) {
        const SoundCategoryLink& link = chain.links[index];
        if (link.key == 0 || link.passiveCount > MaxCategoryPassiveMixes) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 같은 키가 두 번이면 순환이다.
        for (uint32 earlier = 0; earlier < index; ++earlier) {
            if (chain.links[earlier].key == link.key) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (auto valid = ValidateProperties(link.properties); !valid) {
            return valid;
        }

        for (uint32 passive = 0; passive < link.passiveCount; ++passive) {
            const SoundCategoryPassiveMixValue& value = link.passive[passive];
            if (!value.mix || !NonNegativeFinite(value.minVolumeThreshold) ||
                !NonNegativeFinite(value.maxVolumeThreshold) ||
                value.minVolumeThreshold > value.maxVolumeThreshold) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto valid = ValidateMixSnapshotData(*value.mix.Get()); !valid) {
                return valid;
            }
        }
    }

    return Result<void>::Success();
}

SoundLoadingBehavior ChainLoadingBehavior(const SoundCategoryChain& chain) noexcept {
    for (uint32 index = 0; index < chain.depth; ++index) {
        if (chain.links[index].properties.loadingBehavior != SoundLoadingBehavior::Inherited) {
            return chain.links[index].properties.loadingBehavior;
        }
    }

    return SoundLoadingBehavior::Inherited;
}

Result<SoundCategoryChainRef>
SoundCategory::MakeChain(Allocator& allocator, ObjectRegistry& registry,
                         const ObjectAccessContext& access) const noexcept {
    using Made = Result<SoundCategoryChainRef>;
    auto made = SharedPtr<SoundCategoryChain>::TryMake(allocator);
    if (!made) {
        return Made::Failure(made.Error());
    }

    SoundCategoryChain& chain = *made.Value().Get();
    if (auto filled = FillLink(*this, allocator, registry, access, chain.links[0]); !filled) {
        return Made::Failure(filled.Error());
    }

    chain.depth = 1;
    ObjectHandle next = parent.Handle();
    while (!next.IsNull()) {
        // 순환이면 같은 키를 다시 만난다(깊이 상한 전에 거부한다).
        const uint64 key = AudioObjectKey(next);
        if (chain.Contains(key)) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        if (chain.depth >= MaxSoundCategoryDepth) {
            return Made::Failure(ErrorCode::CapacityExceeded);
        }

        auto resolved = registry.Resolve<SoundCategory>(access, next);
        if (!resolved) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto category = resolved.Value().Get();
        if (!category) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        if (auto filled =
                FillLink(*category.Value(), allocator, registry, access, chain.links[chain.depth]);
            !filled) {
            return Made::Failure(filled.Error());
        }

        ++chain.depth;
        next = category.Value()->parent.Handle();
    }

    // passive 임계 등 전체 규칙을 지금 본다(Play 때가 아니라 만들 때 실패하게).
    if (auto valid = ValidateSoundCategoryChain(chain); !valid) {
        return Made::Failure(valid.Error());
    }

    // passive 믹스가 자기 사슬의 분류를 조정하면 되먹임이 생길 수 있다(UE 에디터 경고 —
    // SoundMix.cpp:83–130, 막지는 않는다).
    for (uint32 index = 0; index < chain.depth; ++index) {
        const SoundCategoryLink& link = chain.links[index];
        for (uint32 passive = 0; passive < link.passiveCount; ++passive) {
            const MixSnapshotData& mix = *link.passive[passive].mix.Get();
            for (uint32 adjuster = 0; adjuster < mix.adjusterCount; ++adjuster) {
                if (chain.Contains(mix.adjusters[adjuster].categoryKey)) {
                    Log::Warning(LogAudioCategories,
                                 "Passive sound mix adjusts a category in its own chain; the "
                                 "mix can feed back into its own activation");
                    break;
                }
            }
        }
    }

    return Made::Success(made.Value());
}

Result<SoundLoadingBehavior>
SoundSource::CategoryLoadingBehavior(ObjectRegistry& registry,
                                     const ObjectAccessContext& access) const noexcept {
    using Made = Result<SoundLoadingBehavior>;
    ObjectHandle next = category.Handle();
    for (uint32 depth = 0; !next.IsNull(); ++depth) {
        if (depth >= MaxSoundCategoryDepth) {
            return Made::Failure(ErrorCode::CapacityExceeded);
        }

        auto resolved = registry.Resolve<SoundCategory>(access, next);
        if (!resolved) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto object = resolved.Value().Get();
        if (!object) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        if (object.Value()->loadingBehavior != SoundLoadingBehavior::Inherited) {
            return Made::Success(object.Value()->loadingBehavior);
        }

        next = object.Value()->parent.Handle();
    }

    return Made::Success(SoundLoadingBehavior::Inherited);
}

Result<MixSnapshotRef> MixSnapshot::MakeRef(Allocator& allocator, ObjectRegistry& registry,
                                            const ObjectAccessContext& access) const noexcept {
    using Made = Result<MixSnapshotRef>;
    if (adjusters.Size() > MaxMixAdjusters) {
        return Made::Failure(ErrorCode::CapacityExceeded);
    }

    auto made = SharedPtr<MixSnapshotData>::TryMake(allocator);
    if (!made) {
        return Made::Failure(made.Error());
    }

    MixSnapshotData& data = *made.Value().Get();
    data.key = AudioObjectKey(Handle());
    data.initialDelay = initialDelay;
    data.fadeInTime = fadeInTime;
    data.duration = duration;
    data.fadeOutTime = fadeOutTime;
    for (usize index = 0; index < adjusters.Size(); ++index) {
        const MixCategoryAdjuster& adjuster = adjusters[index];
        // 조정 분류가 살아 있는지 본다(키만 싣는다).
        auto category = registry.Resolve<SoundCategory>(access, adjuster.category.Handle());
        if (!category || !category.Value().Get()) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        MixAdjusterValue& value = data.adjusters[data.adjusterCount++];
        value.categoryKey = AudioObjectKey(adjuster.category.Handle());
        value.volume = adjuster.volume;
        value.pitch = adjuster.pitch;
        value.lowPassFrequency = adjuster.lowPassFrequency;
        value.applyToChildren = adjuster.applyToChildren;
    }

    if (auto valid = ValidateMixSnapshotData(data); !valid) {
        return Made::Failure(valid.Error());
    }

    return Made::Success(made.Value());
}

} // namespace rocks
