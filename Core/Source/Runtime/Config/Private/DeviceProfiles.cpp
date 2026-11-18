#include "ConfigState.h"
#include "ConfigText.h"

#include <Config/DeviceProfiles.h>
#include <Config/DeviceProfileSettings.generated.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

namespace rocks::config {

using detail::ConfigState;

namespace {

// 프로필 오류 진단을 남긴다.
Result<void> ReportProfile(ConfigState& state, StringView path, StringView message) noexcept {
    Log::Warning(detail::LogConfig, "Device profile {Path}: {Message}", path, message);
    return detail::Report(state, ConfigDiagnosticCode::InvalidDeviceProfile, ConfigSeverity::Error,
                          ConfigLayer::DeviceProfile, DeviceProfileTag, nullptr, path, message);
}

bool AppliesTo(const DeviceProfileDefinition& profile, StringView platform) noexcept {
    if (profile.platforms.IsEmpty()) {
        return true;
    }

    for (const String& candidate : profile.platforms) {
        if (candidate.View() == platform) {
            return true;
        }
    }

    return false;
}

// 이름이 정의에 있으면 그대로 고르고, 없으면 진단을 남긴다. 반환: 골랐으면 true.
Result<bool> Pick(ConfigState& state, const DeviceProfileSettings& settings, StringView name,
                  DeviceProfileSource source, DeviceProfileSelection& out) noexcept {
    if (name.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    if (settings.profiles.Find(name) == nullptr) {
        StringBuilder<128> message(state.allocator);
        static_cast<void>(message.TryAppendFormat(
            "the {} profile is not defined; the next selection step is used",
            DeviceProfileSourceName(source)));
        if (auto reported = ReportProfile(state, name, message.View()); !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    if (auto assigned = out.profile.TryAssign(name); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    out.source = source;
    return Result<bool>::Success(true);
}

// 선택 순서대로 프로필을 고른다.
Result<void> Select(ConfigState& state, const DeviceProfileSettings& settings,
                    StringView commandLine, const DeviceTraits& traits,
                    DeviceProfileSelection& out) noexcept {
    out.profile.Clear();
    out.source = DeviceProfileSource::None;
    auto picked = Pick(state, settings, commandLine, DeviceProfileSource::CommandLine, out);
    if (!picked || picked.Value()) {
        return picked ? Result<void>::Success() : Result<void>::Failure(picked.Error());
    }

    picked = Pick(state, settings, settings.profile.View(), DeviceProfileSource::Project, out);
    if (!picked || picked.Value()) {
        return picked ? Result<void>::Success() : Result<void>::Failure(picked.Error());
    }

    const StringView platform = state.platform.View();
    if (state.mode != ConfigMode::Sealed) {
        // 규칙: 이 플랫폼의 후보 가운데 matchPriority가 크고 이름이 앞선 것.
        const String* best = nullptr;
        int32 bestPriority = 0;
        String reason(state.allocator);
        for (const auto& entry : settings.profiles) {
            const DeviceProfileDefinition& profile = entry.value;
            if (profile.match.IsEmpty() || !AppliesTo(profile, platform)) {
                continue;
            }

            auto matched = MatchDeviceConditions(traits, profile.match.View(), reason);
            if (!matched) {
                return Result<void>::Failure(matched.Error());
            }

            if (!matched.Value()) {
                continue;
            }

            const bool better = best == nullptr || profile.matchPriority > bestPriority ||
                                (profile.matchPriority == bestPriority &&
                                 detail::LessBytes(entry.key.View(), best->View()));
            if (better) {
                best = &entry.key;
                bestPriority = profile.matchPriority;
            }
        }

        if (best != nullptr) {
            out.source = DeviceProfileSource::Rule;
            return out.profile.TryAssign(best->View());
        }
    }

    if (platform.IsEmpty()) {
        return Result<void>::Success();
    }

    const String* configured = settings.platformDefaults.Find(platform);
    if (configured != nullptr) {
        picked =
            Pick(state, settings, configured->View(), DeviceProfileSource::PlatformDefault, out);
        return picked ? Result<void>::Success() : Result<void>::Failure(picked.Error());
    }

    // 정의되지 않은 플랫폼 이름 프로필은 오류가 아니다(프로필 없이 돈다).
    if (settings.profiles.Find(platform) != nullptr) {
        out.source = DeviceProfileSource::PlatformDefault;
        return out.profile.TryAssign(platform);
    }

    return Result<void>::Success();
}

// 값 하나를 아직 없으면 더한다(먼저 넣은 쪽이 이긴다 — 자식부터 펼친다).
Result<void> AddIfAbsent(Array<String>& targets, Array<String>& values, StringView target,
                         StringView value) noexcept {
    for (const String& existing : targets) {
        if (existing.View() == target) {
            return Result<void>::Success();
        }
    }

    String targetCopy(targets.GetAllocator());
    String valueCopy(values.GetAllocator());
    Result<void> copied = targetCopy.TryAssign(target);
    if (copied) {
        copied = valueCopy.TryAssign(value);
    }

    if (copied) {
        copied = targets.TryPushBack(Move(targetCopy));
    }

    if (copied) {
        copied = values.TryPushBack(Move(valueCopy));
    }

    return copied;
}

} // namespace

Result<void> RegisterDeviceProfileSettings(ConfigContext& context) noexcept {
    return context.RegisterSettings<DeviceProfileSettings>();
}

StringView DeviceProfileSourceName(DeviceProfileSource source) noexcept {
    switch (source) {
    case DeviceProfileSource::None:
        return "None";
    case DeviceProfileSource::CommandLine:
        return "CommandLine";
    case DeviceProfileSource::Project:
        return "Project";
    case DeviceProfileSource::Rule:
        return "Rule";
    case DeviceProfileSource::PlatformDefault:
        return "PlatformDefault";
    }

    return {};
}

StringView FindDeviceProfileArgument(Span<const StringView> arguments) noexcept {
    StringView found;
    for (const StringView argument : arguments) {
        if (argument.StartsWith(DeviceProfileArgumentPrefix, StringComparison::CaseSensitive)) {
            found =
                argument
                    .TryRightBytes(argument.ByteCount() - DeviceProfileArgumentPrefix.ByteCount())
                    .Value();
        }
    }

    return found;
}

Result<void> ApplyDeviceProfile(ConfigContext& context, StringView commandLineProfile,
                                const DeviceTraits& traits, DeviceProfileSelection& out) noexcept {
    ConfigState* state = context.State();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const void* found = context.FindSettings(GeneratedType<DeviceProfileSettings>::Id());
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const auto& settings = *static_cast<const DeviceProfileSettings*>(found);
    out.values = 0;
    if (auto selected = Select(*state, settings, commandLineProfile, traits, out); !selected) {
        return selected;
    }

    // 메모리 등급: 큰 것부터 시스템 메모리가 하한 이상인 첫 등급.
    out.memoryBucket.Clear();
    DeviceTrait memory;
    if (traits.Find(DeviceTraitNames::SystemMemoryMiB, memory) && memory.numeric) {
        for (const MemoryBucketDefinition& bucket : settings.memoryBuckets) {
            if (memory.number >= static_cast<double>(bucket.minimumMiB)) {
                if (auto assigned = out.memoryBucket.TryAssign(bucket.name.View()); !assigned) {
                    return assigned;
                }

                break;
            }
        }
    }

    // 부모 사슬을 자식부터 펼친다(등급 값 → 일반 값, 아직 없는 대상만).
    Array<String> targets(state->allocator);
    Array<String> values(state->allocator);
    Array<StringView> visited(state->allocator);
    StringView current = out.profile.View();
    while (!current.IsEmpty()) {
        bool cycle = false;
        for (const StringView name : visited) {
            cycle = cycle || name == current;
        }

        const DeviceProfileDefinition* profile = settings.profiles.Find(current);
        if (cycle || profile == nullptr) {
            if (auto reported =
                    ReportProfile(*state, current,
                                  cycle ? "the parent chain has a cycle; it stops here"
                                        : "the parent profile is not defined; the chain "
                                          "stops here");
                !reported) {
                return reported;
            }

            break;
        }

        if (auto pushed = visited.TryPushBack(current); !pushed) {
            return pushed;
        }

        if (!out.memoryBucket.IsEmpty()) {
            const ConfigValueSet* bucket = profile->memoryValues.Find(out.memoryBucket.View());
            if (bucket != nullptr) {
                for (const auto& value : bucket->values) {
                    if (auto added =
                            AddIfAbsent(targets, values, value.key.View(), value.value.View());
                        !added) {
                        return added;
                    }
                }
            }
        }

        for (const auto& value : profile->values) {
            if (auto added = AddIfAbsent(targets, values, value.key.View(), value.value.View());
                !added) {
                return added;
            }
        }

        current = profile->parent.View();
    }

    // 층을 다시 쓴다. 뿌리 쪽 값부터 넣는다(같은 필드를 다른 철자로 가리키면 자식이 이긴다). 빼기와
    // 새 값을 한꺼번에 큐에 넣으므로 반쯤 적용되는 일이 없다(거부된 값만 빠진다).
    Array<SharedPtr<detail::QueuedChange>> changes(state->allocator);
    if (auto reserved = changes.TryReserve(targets.Size() + 1); !reserved) {
        return reserved;
    }

    SharedPtr<detail::QueuedChange> removal;
    if (auto prepared =
            detail::PrepareUnset(*state, ConfigLayer::DeviceProfile, DeviceProfileTag, removal);
        !prepared) {
        return prepared;
    }

    static_cast<void>(changes.TryPushBack(Move(removal))); // 예약했다
    usize accepted = 0;
    for (usize index = targets.Size(); index-- > 0;) {
        SharedPtr<detail::QueuedChange> change;
        auto added = detail::PrepareAssignment(*state, ConfigLayer::DeviceProfile, DeviceProfileTag,
                                               targets[index].View(), values[index].View(), change);
        if (!added) {
            if (added.Error() != ErrorCode::InvalidArgument) {
                return added;
            }

            Log::Warning(detail::LogConfig,
                         "Device profile {Profile} value {Target}={Value} was rejected",
                         out.profile.View(), targets[index].View(), values[index].View());
            continue;
        }

        static_cast<void>(changes.TryPushBack(Move(change))); // 예약했다
        ++accepted;
    }

    if (auto queued = detail::Enqueue(*state->channel, changes.View(), false); !queued) {
        return queued;
    }

    out.values += static_cast<uint32>(accepted);

    Log::Debug(
        detail::LogConfig,
        "Device profile {Profile} selected by {Source} (memory bucket '{Bucket}', {Values} values)",
        out.profile.View(), DeviceProfileSourceName(out.source), out.memoryBucket.View(),
        out.values);
    return Result<void>::Success();
}

} // namespace rocks::config
