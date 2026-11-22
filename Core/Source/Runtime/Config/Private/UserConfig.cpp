#include "ConfigState.h"
#include "ConfigText.h"

#include <Config/UserConfig.h>
#include <Core/FileError.h>
#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

namespace rocks::config {

using detail::ConfigEntry;
using detail::ConfigState;
using detail::ConsoleSettingState;
using detail::EntryKind;
using detail::SectionResult;
using detail::SectionState;

namespace {

// 독립 콘솔 설정(설정 struct 필드가 아닌 것)은 branch가 없으므로 이 branch의 사용자 파일
// `Settings`에 저장한다.
constexpr StringView UserConsoleSettingsBranch = "Engine";

Span<const char> UserConfigToSpan(StringView text) noexcept {
    return Span<const char>(text.Data(), text.ByteCount());
}

StringView UserConfigToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

Result<void> CheckOwner(const ConfigState* state) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    return Result<void>::Success();
}

// ---- 관대한 읽기 ----

// 사용자 파일 하나의 관대한 읽기 상태다. 받은 조각을 writer로 다시 모은다.
class UserFileReader {
public:
    UserFileReader(ConfigState& state, usize branch, StringView sourceName) noexcept
        : state_(state), branch_(branch), sourceName_(sourceName) {
    }

    // 조각 하나를 버리며 Warning을 남긴다.
    Result<void> Drop(ConfigDiagnosticCode code, JsonValue at, StringView path,
                      StringView message) noexcept {
        const JsonPosition position = at.IsValid() ? at.Position() : JsonPosition{0, 0, 0};
        return detail::Report(state_, code, ConfigSeverity::Warning, ConfigLayer::User, sourceName_,
                              at.IsValid() ? &position : nullptr, path, message);
    }

    // `{ "<section>": { "<key>": value } }`(section이 비면 `{ "<key>": value }`) 조각을 사용자 층
    // 입력으로 검사한다. 거부되면 검사 진단을 버리고 첫 Error의 종류·문장을 at 위치의 Warning으로
    // 남긴다. 반환: 받으면 true.
    Result<bool> CheckPiece(StringView section, StringView key, JsonValue value,
                            StringView path) noexcept {
        StringBuilder<256> text(state_.allocator);
        JsonWriter writer(text);
        static_cast<void>(writer.BeginObject());
        static_cast<void>(writer.Key(section));
        static_cast<void>(writer.BeginObject());
        static_cast<void>(writer.Key(key));
        static_cast<void>(writer.Value(value));
        static_cast<void>(writer.EndObject());
        static_cast<void>(writer.EndObject());
        if (auto finished = writer.Finish(); !finished) {
            return Result<bool>::Failure(finished.Error());
        }

        auto document = UniquePtr<JsonDocument>::TryMake(state_.allocator, state_.allocator);
        if (!document) {
            return Result<bool>::Failure(document.Error());
        }

        if (!document.Value()->Parse(text.View()).Succeeded()) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        ConfigEntry entry(state_.allocator);
        entry.kind = EntryKind::Document;
        entry.layer = ConfigLayer::User;
        entry.branch = branch_;
        entry.document = Move(document.Value());
        if (auto assigned = entry.sourceName.TryAssign(sourceName_); !assigned) {
            return Result<bool>::Failure(assigned.Error());
        }

        const ConfigDiagnosticsMark mark = state_.diagnostics.Mark();
        auto valid = detail::ValidateEntry(state_, entry);
        if (!valid) {
            state_.diagnostics.Rewind(mark);
            return valid;
        }

        if (valid.Value()) {
            return Result<bool>::Success(true);
        }

        // 첫 Error를 옮겨 둔다(Rewind가 그 텍스트를 버린다).
        ConfigDiagnostic first;
        first.code = ConfigDiagnosticCode::InvalidSetting;
        String message(state_.allocator);
        for (usize index = mark.count; index < state_.diagnostics.Count(); ++index) {
            const ConfigDiagnostic item = state_.diagnostics.Get(index);
            if (item.severity == ConfigSeverity::Error) {
                first = item;
                if (auto copied = message.TryAssign(item.message); !copied) {
                    state_.diagnostics.Rewind(mark);
                    return Result<bool>::Failure(copied.Error());
                }

                break;
            }
        }

        state_.diagnostics.Rewind(mark);
        ConfigDiagnostic warning;
        warning.code = first.code;
        warning.textCode = first.textCode;
        warning.severity = ConfigSeverity::Warning;
        warning.layer = ConfigLayer::User;
        warning.source = sourceName_;
        warning.position = value.Position();
        warning.path = path;
        warning.message = message.View();
        if (auto added = state_.diagnostics.Add(warning); !added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(false);
    }

    // 원문 root를 걸어 받은 조각을 out에 다시 쓴다. 반환: 받은 조각 수.
    Result<uint32> Read(JsonValue root, JsonWriter& out) noexcept {
        uint32 kept = 0;
        bool versioned = false;
        static_cast<void>(out.BeginObject());
        for (usize member = 0; member < root.Size(); ++member) {
            const StringView key = root.KeyAt(member);
            const JsonValue value = root.ValueAt(member);
            bool duplicate = false;
            for (usize other = 0; other < member && !duplicate; ++other) {
                duplicate = EqualsIgnoringAsciiCase(UserConfigToSpan(root.KeyAt(other)),
                                                    UserConfigToSpan(key));
            }

            Result<void> step = Result<void>::Success();
            if (key == UserConfigVersionKey) {
                versioned = true;
                const auto number = value.TryGetInt64();
                const bool current = number && number.Value() == UserConfigSchemaVersion;
                if (!current) {
                    step = Drop(ConfigDiagnosticCode::UserFileVersion, value, key,
                                "the user file schema version differs from this build; keys "
                                "are checked one by one and unknown ones are dropped");
                }
            } else if (duplicate) {
                step = Drop(ConfigDiagnosticCode::InvalidSection, value, key,
                            "the section appears twice (section keys ignore ASCII case); the "
                            "later one is dropped");
            } else if (!value.IsObject()) {
                step = Drop(ConfigDiagnosticCode::InvalidSection, value, key,
                            "a section value is a JSON object; dropped");
            } else if (EqualsIgnoringAsciiCase(UserConfigToSpan(key),
                                               UserConfigToSpan(ConsoleSettingsSection))) {
                auto read = ReadSettings(key, value, out);
                step = read ? Result<void>::Success() : Result<void>::Failure(read.Error());
                kept += read ? read.Value() : 0;
            } else if (state_.FindSection(branch_, key) == IndexNone) {
                step = Drop(ConfigDiagnosticCode::UnknownSection, value, key,
                            "no settings struct is registered for this section; dropped");
            } else {
                auto read = ReadSection(key, value, out);
                step = read ? Result<void>::Success() : Result<void>::Failure(read.Error());
                kept += read ? read.Value() : 0;
            }

            if (!step) {
                return Result<uint32>::Failure(step.Error());
            }
        }

        if (!versioned) {
            if (auto dropped = Drop(ConfigDiagnosticCode::UserFileVersion, JsonValue{}, {},
                                    "the user file has no $version; keys are checked one by one");
                !dropped) {
                return Result<uint32>::Failure(dropped.Error());
            }
        }

        if (auto ended = out.EndObject(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(kept);
    }

private:
    // 받은 멤버를 out의 key 객체에 모은다. 하나도 없으면 key를 쓰지 않는다.
    template <class Check>
    Result<uint32> ReadMembers(StringView key, JsonValue object, JsonWriter& out,
                               Check&& check) noexcept {
        uint32 kept = 0;
        for (usize member = 0; member < object.Size(); ++member) {
            const StringView name = object.KeyAt(member);
            const JsonValue value = object.ValueAt(member);
            auto accepted = check(name, value);
            if (!accepted) {
                return Result<uint32>::Failure(accepted.Error());
            }

            if (!accepted.Value()) {
                continue;
            }

            if (kept == 0) {
                static_cast<void>(out.Key(key));
                static_cast<void>(out.BeginObject());
            }

            static_cast<void>(out.Key(name));
            static_cast<void>(out.Value(value));
            ++kept;
        }

        if (kept != 0) {
            static_cast<void>(out.EndObject());
        }

        return Result<uint32>::Success(kept);
    }

    Result<uint32> ReadSettings(StringView key, JsonValue object, JsonWriter& out) noexcept {
        return ReadMembers(
            key, object, out, [&](StringView name, JsonValue value) noexcept -> Result<bool> {
                const usize index = state_.FindSetting(name);
                if (!IsConsoleSettingName(name) || index == IndexNone) {
                    auto dropped = Drop(ConfigDiagnosticCode::UnknownSetting, value, name,
                                        "no console setting is registered with "
                                        "this name; dropped");
                    return dropped ? Result<bool>::Success(false)
                                   : Result<bool>::Failure(dropped.Error());
                }

                return CheckPiece(ConsoleSettingsSection, name, value, name);
            });
    }

    Result<uint32> ReadSection(StringView key, JsonValue object, JsonWriter& out) noexcept {
        return ReadMembers(key, object, out,
                           [&](StringView name, JsonValue value) noexcept -> Result<bool> {
                               return CheckPiece(key, name, value, name);
                           });
    }

    ConfigState& state_;
    usize branch_;
    StringView sourceName_;
};

// ---- 저장 ----

bool BelowUserLayer(const ConfigEntry& entry) noexcept {
    return entry.layer < ConfigLayer::User;
}

bool UpToUserLayer(const ConfigEntry& entry) noexcept {
    return entry.layer <= ConfigLayer::User;
}

// 사용자 층이 쓸 수 있는 설정 필드 플래그인지다(출시 규칙의 DevelopmentOnly는 저장하지 않는다).
bool Savable(const ConfigState& state, uint32 flags) noexcept {
    return HasFlag(flags, SettingsFieldFlags::UserConfig) &&
           !(state.releaseRules && HasFlag(flags, SettingsFieldFlags::DevelopmentOnly));
}

// 섹션 하나의 차이(바뀐 UserConfig 필드)를 writer에 쓴다. 반환: 쓴 필드 수.
Result<uint32> WriteSectionDifference(ConfigState& state, usize sectionIndex,
                                      Span<ConfigEntry* const> below, Span<ConfigEntry* const> upTo,
                                      JsonWriter& writer) noexcept {
    SectionState& section = *state.sections[sectionIndex];
    bool any = false;
    for (const SettingsFieldSchema& field : section.ops.settings->fields) {
        any = any || Savable(state, field.flags);
    }

    if (!any) {
        return Result<uint32>::Success(0);
    }

    SectionResult lower(state.allocator);
    SectionResult user(state.allocator);
    auto release = MakeScopeExit([&]() noexcept {
        if (lower.value != nullptr) {
            section.ops.destroy(state.allocator, lower.value);
        }

        if (user.value != nullptr) {
            section.ops.destroy(state.allocator, user.value);
        }
    });
    if (auto computed = detail::RecomputeSection(state, section, sectionIndex, below, lower);
        !computed) {
        return Result<uint32>::Failure(computed.Error());
    }

    if (auto computed = detail::RecomputeSection(state, section, sectionIndex, upTo, user);
        !computed) {
        return Result<uint32>::Failure(computed.Error());
    }

    String lowerText(state.allocator);
    String userText(state.allocator);
    if (auto exported = detail::ExportSection(state, section, lower.value, lowerText); !exported) {
        return Result<uint32>::Failure(exported.Error());
    }

    if (auto exported = detail::ExportSection(state, section, user.value, userText); !exported) {
        return Result<uint32>::Failure(exported.Error());
    }

    JsonDocument lowerDocument(state.allocator);
    JsonDocument userDocument(state.allocator);
    if (!lowerDocument.Parse(lowerText.View()).Succeeded() ||
        !userDocument.Parse(userText.View()).Succeeded()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 스키마 필드 순서로 쓴다(ExportJson과 같은 순서).
    uint32 written = 0;
    for (const FieldSchema& schema : section.lease.Schema().fields) {
        if (!Savable(state, detail::SettingsFlagsOf(section, schema.id))) {
            continue;
        }

        const StringView tool = UserConfigToView(EffectiveToolName(schema));
        const JsonValue before = lowerDocument.Root().Find(tool);
        const JsonValue after = userDocument.Root().Find(tool);
        if (!after.IsValid() || (before.IsValid() && JsonEquals(before, after))) {
            continue;
        }

        if (written == 0) {
            static_cast<void>(writer.Key(section.section.View()));
            static_cast<void>(writer.BeginObject());
        }

        static_cast<void>(writer.Key(tool));
        static_cast<void>(writer.Value(after));
        ++written;
    }

    if (written != 0) {
        if (auto ended = writer.EndObject(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }
    }

    return Result<uint32>::Success(written);
}

// 콘솔 설정 값을 그 종류의 JSON 값으로 쓴다(읽기 ConsoleTextOfJson의 역).
Result<void> WriteConsoleValue(JsonWriter& writer, const ConsoleSettingState& setting,
                               const FieldValue& value, StringView text) noexcept {
    switch (setting.kind) {
    case ValueKind::Bool:
        return writer.Bool(value.boolean);
    case ValueKind::Int32:
    case ValueKind::Int64:
        return writer.Int64(value.signedInteger);
    case ValueKind::Float32:
        return writer.Float(value.float32);
    case ValueKind::Float64:
        return writer.Double(value.float64);
    default:
        return writer.String(text);
    }
}

// 독립 콘솔 설정 가운데 UserConfig인 것의 차이를 writer의 `Settings`에 쓴다. 반환: 쓴 수.
Result<uint32> WriteSettingsDifference(ConfigState& state, Span<ConfigEntry* const> below,
                                       Span<ConfigEntry* const> upTo, JsonWriter& writer) noexcept {
    Array<const ConsoleSettingState*> settings(state.allocator);
    for (const UniquePtr<ConsoleSettingState>& setting : state.settings) {
        const bool hidden =
            state.releaseRules && HasFlag(setting->flags, ConsoleSettingFlags::DevelopmentOnly);
        if (setting->section != IndexNone ||
            !HasFlag(setting->flags, ConsoleSettingFlags::UserConfig) || hidden) {
            continue;
        }

        if (auto pushed = settings.TryPushBack(setting.Get()); !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }
    }

    settings.Sort([](const ConsoleSettingState* left, const ConsoleSettingState* right) noexcept {
        return detail::LessBytes(left->name.View(), right->name.View());
    });
    uint32 written = 0;
    for (const ConsoleSettingState* setting : settings) {
        FieldValue lowerValue(state.allocator);
        FieldValue userValue(state.allocator);
        String lowerText(state.allocator);
        String userText(state.allocator);
        Array<uint64> sources(state.allocator);
        if (auto computed = detail::RecomputeConsoleSetting(state, *setting, below, false,
                                                            lowerValue, lowerText, sources);
            !computed) {
            return Result<uint32>::Failure(computed.Error());
        }

        if (auto computed = detail::RecomputeConsoleSetting(state, *setting, upTo, false, userValue,
                                                            userText, sources);
            !computed) {
            return Result<uint32>::Failure(computed.Error());
        }

        if (lowerText.View() == userText.View()) {
            continue;
        }

        if (written == 0) {
            static_cast<void>(writer.Key(ConsoleSettingsSection));
            static_cast<void>(writer.BeginObject());
        }

        static_cast<void>(writer.Key(setting->name.View()));
        if (auto value = WriteConsoleValue(writer, *setting, userValue, userText.View()); !value) {
            return Result<uint32>::Failure(value.Error());
        }

        ++written;
    }

    if (written != 0) {
        if (auto ended = writer.EndObject(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }
    }

    return Result<uint32>::Success(written);
}

// content를 path에 원자적으로 쓴다(임시 파일 → Flush → Replace).
Result<void> WriteAtomically(platform::FileSystem& files, StringView path, StringView content,
                             Allocator& allocator) noexcept {
    StringBuilder<256> temporary(allocator);
    static_cast<void>(temporary.TryAppendFormat("{}.tmp", path));
    if (auto status = temporary.Status(); !status) {
        return status;
    }

    platform::File file;
    platform::FileStatus status =
        file.Open(files, temporary.View().Data(), temporary.View().ByteCount(),
                  platform::FileOpenMode::CreateAlways);
    if (!status) {
        return Result<void>::Failure(FileErrorCode(status));
    }

    status = file.WriteAll(content.Data(), content.ByteCount()).status;
    if (status) {
        status = file.Flush();
    }

    const platform::FileStatus closed = file.Close();
    if (status && !closed) {
        status = closed;
    }

    if (status) {
        status = files.Replace(temporary.View().Data(), temporary.View().ByteCount(), path.Data(),
                               path.ByteCount());
    }

    if (!status) {
        static_cast<void>(files.Delete(temporary.View().Data(), temporary.View().ByteCount()));
        return Result<void>::Failure(FileErrorCode(status));
    }

    return Result<void>::Success();
}

} // namespace

Result<void> AddUserFileText(ConfigContext& context, StringView tag, StringView branch,
                             StringView sourceName, StringView text, bool replace) noexcept {
    ConfigState* state = context.State();
    if (auto owner = CheckOwner(state); !owner) {
        return owner;
    }

    const usize branchIndex = state->FindBranch(branch);
    if (branchIndex == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    UserFileReader reader(*state, branchIndex, sourceName);
    JsonDocument document(state->allocator);
    JsonReadOptions read;
    read.allowLineComments = true;
    const JsonError error = document.Parse(text, read);
    StringBuilder<1024> kept(state->allocator);
    uint32 pieces = 0;
    if (!error.Succeeded()) {
        if (error.code == JsonErrorCode::OutOfMemory) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        const JsonPosition at = error.position;
        if (auto reported = detail::Report(*state, ConfigDiagnosticCode::JsonSyntax,
                                           ConfigSeverity::Warning, ConfigLayer::User, sourceName,
                                           &at, {}, JsonErrorMessage(error.code));
            !reported) {
            return reported;
        }
    } else if (!document.Root().IsObject()) {
        if (auto dropped = reader.Drop(ConfigDiagnosticCode::InvalidSection, document.Root(), {},
                                       "a user file is a JSON object of sections; dropped");
            !dropped) {
            return dropped;
        }
    } else {
        JsonWriter writer(kept);
        auto collected = reader.Read(document.Root(), writer);
        if (!collected) {
            return Result<void>::Failure(collected.Error());
        }

        if (auto finished = writer.Finish(); !finished) {
            return finished;
        }

        pieces = collected.Value();
    }

    if (pieces == 0) {
        return replace ? context.Unset(ConfigLayer::User, tag) : Result<void>::Success();
    }

    return replace
               ? context.ReplaceLayerText(ConfigLayer::User, tag, branch, sourceName, kept.View())
               : context.AddLayerText(ConfigLayer::User, tag, branch, sourceName, kept.View());
}

Result<void> SetUserValue(ConfigContext& context, StringView target, StringView value) noexcept {
    return context.AddAssignment(ConfigLayer::User, UserChangeTag, target, value);
}

Result<uint32> WriteUserConfig(ConfigContext& context, StringView branch,
                               JsonWriter& writer) noexcept {
    ConfigState* state = context.State();
    if (auto owner = CheckOwner(state); !owner) {
        return Result<uint32>::Failure(owner.Error());
    }

    const usize branchIndex = state->FindBranch(branch);
    if (branchIndex == IndexNone) {
        return Result<uint32>::Failure(ErrorCode::NotFound);
    }

    Array<ConfigEntry*> below(state->allocator);
    Array<ConfigEntry*> upTo(state->allocator);
    if (auto sorted = detail::SortedEntries(*state, &BelowUserLayer, below); !sorted) {
        return Result<uint32>::Failure(sorted.Error());
    }

    if (auto sorted = detail::SortedEntries(*state, &UpToUserLayer, upTo); !sorted) {
        return Result<uint32>::Failure(sorted.Error());
    }

    // 다시 합치는 동안 거부되는 입력의 진단은 이미 Sync가 남겼으므로 버린다.
    const ConfigDiagnosticsMark mark = state->diagnostics.Mark();
    auto rewind = MakeScopeExit([&]() noexcept { state->diagnostics.Rewind(mark); });
    Array<usize> sections(state->allocator);
    for (usize index = 0; index < state->sections.Size(); ++index) {
        if (state->sections[index]->branch != branchIndex) {
            continue;
        }

        if (auto pushed = sections.TryPushBack(index); !pushed) {
            return Result<uint32>::Failure(pushed.Error());
        }
    }

    sections.Sort([state](usize left, usize right) noexcept {
        return detail::LessBytes(state->sections[left]->section.View(),
                                 state->sections[right]->section.View());
    });
    static_cast<void>(writer.BeginObject());
    static_cast<void>(writer.Key(UserConfigVersionKey));
    static_cast<void>(writer.Int64(UserConfigSchemaVersion));
    uint32 written = 0;
    for (const usize index : sections) {
        auto section = WriteSectionDifference(*state, index, below.View(), upTo.View(), writer);
        if (!section) {
            return section;
        }

        written += section.Value();
    }

    if (branch == UserConsoleSettingsBranch) {
        auto settings = WriteSettingsDifference(*state, below.View(), upTo.View(), writer);
        if (!settings) {
            return settings;
        }

        written += settings.Value();
    }

    if (auto ended = writer.EndObject(); !ended) {
        return Result<uint32>::Failure(ended.Error());
    }

    return Result<uint32>::Success(written);
}

Result<void> UserConfigDirectory(ConfigContext& context, String& out) noexcept {
    ConfigState* state = context.State();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const StringView userData =
        state->directories[static_cast<usize>(ConfigPathToken::UserDataDir)].View();
    if (state->mode != ConfigMode::Standard || userData.IsEmpty() || state->projectName.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    StringBuilder<256> path(state->allocator);
    static_cast<void>(path.TryAppendFormat("{}/{}/Config", userData, state->projectName.View()));
    if (auto status = path.Status(); !status) {
        return status;
    }

    return out.TryAssign(path.View());
}

Result<UserConfigSaveResult> SaveUserConfig(ConfigContext& context,
                                            platform::FileSystem& files) noexcept {
    ConfigState* state = context.State();
    if (auto owner = CheckOwner(state); !owner) {
        return Result<UserConfigSaveResult>::Failure(owner.Error());
    }

    String directory(state->allocator);
    if (auto found = UserConfigDirectory(context, directory); !found) {
        return Result<UserConfigSaveResult>::Failure(found.Error());
    }

    if (const platform::FileStatus made =
            platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        return Result<UserConfigSaveResult>::Failure(FileErrorCode(made));
    }

    UserConfigSaveResult result;
    for (usize branch = 0; branch < state->branches.Size(); ++branch) {
        const StringView name = state->branches[branch].View();
        StringBuilder<1024> text(state->allocator);
        JsonWriter writer(text);
        auto written = WriteUserConfig(context, name, writer);
        if (!written) {
            return Result<UserConfigSaveResult>::Failure(written.Error());
        }

        if (auto finished = writer.Finish(); !finished) {
            return Result<UserConfigSaveResult>::Failure(finished.Error());
        }

        StringBuilder<256> path(state->allocator);
        static_cast<void>(path.TryAppendFormat("{}/{}.json", directory.View(), name));
        if (auto status = path.Status(); !status) {
            return Result<UserConfigSaveResult>::Failure(status.Error());
        }

        if (written.Value() == 0) {
            bool exists = false;
            if (const platform::FileStatus checked =
                    files.FileExists(path.View().Data(), path.View().ByteCount(), exists);
                !checked) {
                return Result<UserConfigSaveResult>::Failure(FileErrorCode(checked));
            }

            if (exists) {
                if (const platform::FileStatus deleted =
                        files.Delete(path.View().Data(), path.View().ByteCount());
                    !deleted) {
                    return Result<UserConfigSaveResult>::Failure(FileErrorCode(deleted));
                }

                ++result.removed;
            }

            continue;
        }

        if (auto saved = WriteAtomically(files, path.View(), text.View(), state->allocator);
            !saved) {
            return Result<UserConfigSaveResult>::Failure(saved.Error());
        }

        ++result.written;
        result.values += written.Value();
    }

    Log::Info(detail::LogConfig, "Saved user config to {Directory}: {Files} files, {Values} values",
              directory.View(), result.written, result.values);
    return Result<UserConfigSaveResult>::Success(result);
}

} // namespace rocks::config
