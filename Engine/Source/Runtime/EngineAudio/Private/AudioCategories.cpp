#include "AudioDeviceContext.h"

#include <Core/Math/MathUtility.h>

// 분류 실효값이다(Docs/Design/AudioSystem 3.43.2·3.43.6절, UE ParseSoundClasses·ApplyClassAdjusters
// — AudioDevice.cpp:2499–2635·3021–3179). 공유 분류 표를 두지 않고 재생마다 수락 때의 사슬로
// 계산한다 (같은 사슬 포인터는 묶음 안에서 한 번만 — 캐시).
namespace rocks {

AudioDeviceContext::CategoryValues
AudioDeviceContext::ComputeCategory(const SoundCategoryChain* chain, uint64 now) noexcept {
    CategoryValues values;
    if (chain == nullptr || chain->depth == 0) {
        return values;
    }

    for (const CategoryCache& cached : categoryCache_) {
        if (cached.chain == chain) {
            return cached.values;
        }
    }

    // 1) 사슬 곱: volume·pitch는 곱, UI·Music은 OR(UE RecurseIntoSoundClasses). 사용자 볼륨은
    // 원소의 자기 볼륨에 곱한다(3.43.3절 — 믹스보다 먼저).
    for (uint32 index = 0; index < chain->depth; ++index) {
        const SoundCategoryProperties& properties = chain->links[index].properties;
        values.volume *=
            properties.volume * userVolume_.values[static_cast<uint32>(properties.userVolume)];
        values.pitch *= properties.pitch;
        values.isUiSound = values.isUiSound || properties.isUiSound;
        values.isMusic = values.isMusic || properties.isMusic;
    }

    // 나머지는 잎 값만 쓴다(AD 2518–2519).
    const SoundCategoryLink& leaf = chain->links[0];
    values.lowPassFrequency = leaf.properties.lowPassFrequency;
    values.alwaysPlay = leaf.properties.alwaysPlay;
    values.distanceScale = CategoryDistanceScale(leaf.key, leaf.properties, now);

    // 2) 믹스 조정(이번 묶음 보간·override 반영 값): 잎이 키이거나, applyToChildren이고 키가 사슬
    // 안이면 곱한다(UE RecursiveApplyAdjuster와 같은 결과).
    for (const MixAdjustment& adjustment : adjustments_) {
        if (adjustment.categoryKey == leaf.key ||
            (adjustment.applyToChildren && chain->Contains(adjustment.categoryKey))) {
            values.volume *= adjustment.volume;
            values.pitch *= adjustment.pitch;
            values.lowPassFrequency =
                math::Min(values.lowPassFrequency, adjustment.lowPassFrequency);
        }
    }

    if (categoryCache_.Size() < categoryCache_.Capacity()) {
        CategoryCache cached;
        cached.chain = chain;
        cached.values = values;
        (void)categoryCache_.TryPushBack(cached);
    }

    return values;
}

float AudioDeviceContext::CategoryDistanceScale(uint64 key,
                                                const SoundCategoryProperties& properties,
                                                uint64 now) const noexcept {
    // 걸린 값이 있으면 그것, 없으면 에셋 값이다(UE DynamicSoundClassProperties — 3.43.6절).
    for (const CategoryScale& scale : categoryScales_) {
        if (scale.key == key) {
            return math::Max(scale.ramp.At(now), 0.0f);
        }
    }

    return math::Max(properties.attenuationDistanceScale, 0.0f);
}

void AudioDeviceContext::UpdateCategories(uint64 now) noexcept {
    // 믹스·override·거리 배수가 움직이는 동안은 분류 있는 재생을 묶음마다 다시 게시한다(3.43.2절
    // dirty).
    bool ramping = mixesChanging_;
    for (const CategoryScale& scale : categoryScales_) {
        ramping = ramping || scale.ramp.IsRamping(now);
    }

    for (Record& record : records_) {
        if (record.state == RecordState::Inactive) {
            continue;
        }

        const CategoryValues values = ComputeCategory(record.category.Get(), now);
        if (ramping || values.volume != record.categoryValues.volume ||
            values.pitch != record.categoryValues.pitch ||
            values.lowPassFrequency != record.categoryValues.lowPassFrequency ||
            values.distanceScale != record.categoryValues.distanceScale) {
            record.paramsDirty = record.paramsDirty || record.category.Get() != nullptr;
        }

        record.categoryValues = values;
        record.alwaysPlay = record.optionAlwaysPlay || values.alwaysPlay;
        record.isUiSound = record.optionUiSound || values.isUiSound;
    }
}

void AudioDeviceContext::ApplySetCategoryDistanceScale(const ControlCommand& command,
                                                       uint64 now) noexcept {
    const uint32 frames = SecondsToMixFrames(static_cast<double>(command.seconds));
    for (CategoryScale& scale : categoryScales_) {
        if (scale.key == command.categoryKey) {
            scale.ramp.StartFrom(scale.ramp.At(now), command.value, now, frames);
            return;
        }
    }

    // 칸이 없으면 에셋 값에서 시작한다. 칸은 장치 수명 동안 남는다(UE와 같다).
    if (categoryScales_.Size() >= categoryScales_.Capacity()) {
        ++stats_.categoryScaleRejects;
        return;
    }

    CategoryScale scale;
    scale.key = command.categoryKey;
    scale.ramp.Set(command.assetValue);
    scale.ramp.StartFrom(command.assetValue, command.value, now, frames);
    (void)categoryScales_.TryPushBack(scale);
}

} // namespace rocks
