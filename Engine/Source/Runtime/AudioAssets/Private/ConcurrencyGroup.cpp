#include <AudioAssets/AudioObjectKey.h>

#include <AudioAssets/ConcurrencyGroup.generated.h>

namespace rocks {

Result<SoundConcurrencyEntry> ConcurrencyGroup::MakeEntry() const noexcept {
    if (auto valid = ValidateSoundConcurrencySettings(settings); !valid) {
        return Result<SoundConcurrencyEntry>::Failure(valid.Error());
    }

    SoundConcurrencyEntry entry;
    entry.key = AudioObjectKey(Handle());
    entry.isOverride = false;
    entry.settings = settings;
    return Result<SoundConcurrencyEntry>::Success(entry);
}

} // namespace rocks
