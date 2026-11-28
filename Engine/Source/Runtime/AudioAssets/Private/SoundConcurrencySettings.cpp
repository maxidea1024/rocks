#include "AudioAssetsValidation.h"

#include <AudioAssets/SoundConcurrencyEntry.h>
#include <AudioAssets/SoundConcurrencySettings.h>

namespace rocks {

Result<void> ValidateSoundConcurrencySettings(const SoundConcurrencySettings& settings) noexcept {
    using audio_assets_detail::IsNonNegativeFinite;
    const bool valid =
        settings.maxCount >= 1 &&
        static_cast<uint8>(settings.resolutionRule) <=
            static_cast<uint8>(SoundConcurrencyRule::StopLowestPriorityThenPreventNew) &&
        static_cast<uint8>(settings.volumeScaleMode) <=
            static_cast<uint8>(SoundConcurrencyVolumeScaleMode::Priority) &&
        IsNonNegativeFinite(settings.retriggerTime) && IsNonNegativeFinite(settings.volumeScale) &&
        settings.volumeScale <= 1.0f && IsNonNegativeFinite(settings.volumeScaleAttackTime) &&
        IsNonNegativeFinite(settings.volumeScaleReleaseTime) &&
        IsNonNegativeFinite(settings.voiceStealReleaseTime) &&
        settings.voiceStealReleaseTime <= MaxVoiceStealReleaseSeconds;
    return valid ? Result<void>::Success() : Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> ValidateSoundConcurrencyEntries(const SoundConcurrencyEntry* entries,
                                             usize count) noexcept {
    if (count > MaxConcurrencyGroups) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    for (usize index = 0; index < count; ++index) {
        if (entries[index].key == 0 || !ValidateSoundConcurrencySettings(entries[index].settings)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (entries[earlier].key == entries[index].key &&
                entries[earlier].isOverride == entries[index].isOverride) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
