#include <Core/Diagnostics/Log.h>
#include <Core/Text/Json.h>
#include <InputActions/InputActionsLog.h>
#include <InputActions/InputUserSettings.h>

// 사용자 키 설정 저장·불러오기다(Docs/Design/InputActions 5.3절 — UE EnhancedInputUserSettings.cpp
// :948-1128 Serialize). 형식은 JSON(IAD12)이다.
namespace rocks::input {

namespace {

bool SaveIsCorrupt(JsonValue value, bool (JsonValue::*check)() const noexcept) noexcept {
    return !value.IsValid() || !(value.*check)();
}

} // namespace

Result<void> InputUserSettings::Save(StringBuilderBase& out) noexcept {
    JsonWriter writer(out);
    (void)writer.BeginObject();
    (void)writer.Key("version");
    (void)writer.UInt64(InputUserSettingsVersion);
    (void)writer.Key("currentProfile");
    (void)writer.String(CurrentProfileId());
    (void)writer.Key("profiles");
    (void)writer.BeginArray();
    for (const KeyProfile& profile : profiles_) {
        (void)writer.BeginObject();
        (void)writer.Key("id");
        (void)writer.String(profile.id.View());
        (void)writer.Key("displayName");
        (void)writer.String(profile.displayName.View());
        (void)writer.Key("mappings");
        (void)writer.BeginArray();
        // customized이거나 dirty인 매핑만 쓴다(UE :978-994).
        for (const KeyMappingRow& row : profile.rows) {
            for (const PlayerKeyMapping& mapping : row.mappings) {
                if (!mapping.IsCustomized() && !mapping.dirty) {
                    continue;
                }

                (void)writer.BeginObject();
                (void)writer.Key("name");
                (void)writer.String(row.mappingName.View());
                (void)writer.Key("slot");
                (void)writer.UInt64(mapping.slot);
                (void)writer.Key("key");
                (void)writer.String(mapping.currentKey.IsValid() ? InputKeyName(mapping.currentKey)
                                                                 : StringView());
                (void)writer.EndObject();
            }
        }

        (void)writer.EndArray();
        (void)writer.EndObject();
    }

    (void)writer.EndArray();
    (void)writer.EndObject();
    // 쓰기 실패는 첫 오류가 Finish에 남는다(그때 dirty는 그대로다).
    if (auto finished = writer.Finish(); !finished) {
        return finished;
    }

    for (KeyProfile& profile : profiles_) {
        for (KeyMappingRow& row : profile.rows) {
            for (PlayerKeyMapping& mapping : row.mappings) {
                mapping.dirty = false;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> InputUserSettings::Load(StringView text, Allocator& scratch) noexcept {
    JsonDocument document(scratch);
    const JsonError parsed = document.Parse(text);
    const JsonValue root = document.Root();
    if (parsed.code != JsonErrorCode::None || SaveIsCorrupt(root, &JsonValue::IsObject)) {
        return Result<void>::Failure(parsed.code == JsonErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::InvalidArgument);
    }

    const JsonValue version = root.Find("version");
    const JsonValue profiles = root.Find("profiles");
    if (SaveIsCorrupt(version, &JsonValue::IsNumber) ||
        SaveIsCorrupt(profiles, &JsonValue::IsArray)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto versionNumber = version.TryGetUInt64();
    if (!versionNumber) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (versionNumber.Value() > InputUserSettingsVersion) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 모양을 먼저 다 본다(깨졌으면 아무것도 바꾸지 않는다).
    for (usize index = 0; index < profiles.Size(); ++index) {
        const JsonValue profile = profiles.At(index);
        if (SaveIsCorrupt(profile, &JsonValue::IsObject) ||
            SaveIsCorrupt(profile.Find("id"), &JsonValue::IsString) ||
            profile.Find("id").GetString().IsEmpty() ||
            SaveIsCorrupt(profile.Find("mappings"), &JsonValue::IsArray)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const JsonValue mappings = profile.Find("mappings");
        for (usize at = 0; at < mappings.Size(); ++at) {
            const JsonValue mapping = mappings.At(at);
            if (SaveIsCorrupt(mapping, &JsonValue::IsObject) ||
                SaveIsCorrupt(mapping.Find("name"), &JsonValue::IsString) ||
                SaveIsCorrupt(mapping.Find("slot"), &JsonValue::IsNumber) ||
                SaveIsCorrupt(mapping.Find("key"), &JsonValue::IsString)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    // 사본에 덮어쓴 뒤 바꾼다(할당 실패면 그대로).
    Array<KeyProfile> staged(*allocator_);
    if (auto copied = CopyProfiles(staged); !copied) {
        return copied;
    }

    // 덮어쓰기다: 파일은 바꾼 매핑만 담으므로 먼저 모든 매핑을 기본 키로 되돌리고, 기본 키가 없는
    // 매핑(사용자가 만든 slot)과 빈 행을 지운다(UE는 저장본에서 새 객체를 만든다).
    for (KeyProfile& profile : staged) {
        for (usize rowIndex = profile.rows.Size(); rowIndex > 0; --rowIndex) {
            KeyMappingRow& row = profile.rows[rowIndex - 1];
            for (usize at = row.mappings.Size(); at > 0; --at) {
                PlayerKeyMapping& mapping = row.mappings[at - 1];
                if (!mapping.defaultKey.IsValid()) {
                    (void)row.mappings.TryRemoveAt(at - 1);
                    continue;
                }

                mapping.currentKey = mapping.defaultKey;
                mapping.dirty = false;
            }

            if (row.mappings.IsEmpty()) {
                (void)profile.rows.TryRemoveAt(rowIndex - 1);
            }
        }
    }

    for (usize index = 0; index < profiles.Size(); ++index) {
        const JsonValue source = profiles.At(index);
        const StringView id = source.Find("id").GetString();
        KeyProfile* profile = nullptr;
        for (KeyProfile& candidate : staged) {
            profile = candidate.id.View() == id ? &candidate : profile;
        }

        const JsonValue displayName = source.Find("displayName");
        const StringView name = displayName.IsValid() && displayName.IsString()
                                    ? displayName.GetString()
                                    : StringView(DefaultKeyProfileDisplayName);
        if (profile == nullptr) {
            if (auto added = AddProfile(staged, id, name); !added) {
                return added;
            }

            profile = &staged[staged.Size() - 1];
        } else if (auto assigned = profile->displayName.TryAssign(name); !assigned) {
            return assigned;
        }

        const JsonValue mappings = source.Find("mappings");
        for (usize at = 0; at < mappings.Size(); ++at) {
            const JsonValue entry = mappings.At(at);
            const StringView mappingName = entry.Find("name").GetString();
            auto slot = entry.Find("slot").TryGetUInt64();
            // 빈 키 이름은 비운 slot(무효 키)이다(Save가 그렇게 쓴다).
            const StringView keyName = entry.Find("key").GetString();
            Result<InputKey> key =
                keyName.IsEmpty() ? Result<InputKey>::Success(InputKey{}) : ParseInputKey(keyName);
            // 모르는 키 이름·범위 밖 slot은 건너뛴다(설정을 버리지 않는다 — 5.3절).
            if (mappingName.IsEmpty() || !slot || slot.Value() >= KeySlotCount || !key) {
                Log::Warning(LogInputActions,
                             "Input user settings entry '{Name}' of profile '{Profile}' is skipped "
                             "(unknown key or slot) ({Code})",
                             mappingName, id, DiagInputUserSettingsEntrySkipped);
                continue;
            }

            KeyMappingRow* row = nullptr;
            for (KeyMappingRow& candidate : profile->rows) {
                row = candidate.mappingName.View() == mappingName ? &candidate : row;
            }

            if (row == nullptr) {
                KeyMappingRow created(*allocator_);
                if (auto assigned = created.mappingName.TryAssign(mappingName); !assigned) {
                    return assigned;
                }

                if (auto pushed = profile->rows.TryPushBack(Move(created)); !pushed) {
                    return pushed;
                }

                row = &profile->rows[profile->rows.Size() - 1];
            }

            const auto slotValue = static_cast<uint8>(slot.Value());
            PlayerKeyMapping* mapping = nullptr;
            for (PlayerKeyMapping& candidate : row->mappings) {
                mapping = candidate.slot == slotValue ? &candidate : mapping;
            }

            if (mapping != nullptr) {
                mapping->currentKey = key.Value();
                mapping->dirty = false;
                continue;
            }

            // 행이 없던 매핑은 기본 키 없이 만든다(등록이 채운다 — UE와 같다).
            PlayerKeyMapping created;
            created.slot = slotValue;
            created.currentKey = key.Value();
            if (auto pushed = row->mappings.TryPushBack(created); !pushed) {
                return pushed;
            }
        }
    }

    // 현재 프로필: 목록에 없으면 기본 프로필이다.
    usize current = 0;
    const JsonValue currentId = root.Find("currentProfile");
    const StringView wanted = currentId.IsValid() && currentId.IsString()
                                  ? currentId.GetString()
                                  : StringView(DefaultKeyProfileId);
    bool found = false;
    for (usize index = 0; index < staged.Size(); ++index) {
        if (staged[index].id.View() == wanted) {
            current = index;
            found = true;
        }
    }

    if (!found) {
        for (usize index = 0; index < staged.Size(); ++index) {
            current = staged[index].id.View() == StringView(DefaultKeyProfileId) ? index : current;
        }
    }

    profiles_ = Move(staged);
    current_ = current;
    ++revision_;
    return Result<void>::Success();
}

} // namespace rocks::input
