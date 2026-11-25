#include "InputMappingLookup.h"

#include <InputActions/InputActionData.generated.h>
#include <InputActions/InputActionSettings.generated.h>
#include <Core/Diagnostics/Log.h>
#include <InputActions/InputActionsLog.h>
#include <InputActions/InputContextData.generated.h>
#include <InputActions/InputUserSettings.h>
#include <Object/ValueCopy.h>

// 사용자 키 설정(등록·키 바꾸기·프로필)이다(Docs/Design/InputActions 5.2절 — UE
// EnhancedInputUserSettings.cpp:1133-1350·1539-1782).
namespace rocks::input {

namespace {

KeyMappingRow* SettingsFindRow(KeyProfile& profile, StringView name) noexcept {
    for (KeyMappingRow& row : profile.rows) {
        if (row.mappingName.View() == name) {
            return &row;
        }
    }

    return nullptr;
}

const KeyMappingRow* SettingsFindRow(const KeyProfile& profile, StringView name) noexcept {
    for (const KeyMappingRow& row : profile.rows) {
        if (row.mappingName.View() == name) {
            return &row;
        }
    }

    return nullptr;
}

PlayerKeyMapping* SettingsFindSlot(KeyMappingRow& row, uint8 slot) noexcept {
    for (PlayerKeyMapping& mapping : row.mappings) {
        if (mapping.slot == slot) {
            return &mapping;
        }
    }

    return nullptr;
}

const InputActionData* SettingsFindAction(const InputContextBundle& bundle,
                                          const Guid& id) noexcept {
    for (const InputActionData& action : bundle.actions) {
        if (action.id == id) {
            return &action;
        }
    }

    return nullptr;
}

// 프로필 하나의 바인딩 목록이다(override가 있으면 그것 — UE GetMappingsForProfile).
Span<const InputBinding> SettingsProfileBindings(const InputContextBundle& bundle,
                                                 StringView profile) noexcept {
    for (const InputProfileBindings& entry : bundle.data.profileOverrides) {
        if (entry.profile.View() == profile) {
            return entry.bindings.View();
        }
    }

    return bundle.data.bindings.View();
}

Result<void> SettingsCopyRow(KeyMappingRow& out, const KeyMappingRow& source) noexcept {
    if (auto assigned = out.mappingName.TryAssign(source.mappingName.View()); !assigned) {
        return assigned;
    }

    return out.mappings.TryAppend(source.mappings.View());
}

} // namespace

InputUserSettings::InputUserSettings(Allocator& allocator) noexcept
    : allocator_(&allocator), profiles_(allocator), registered_(allocator) {
    // 기본 프로필(UE LoadOrCreateSettings :733-745). 할당 실패면 프로필이 없다(키 바꾸기는
    // NotFound).
    (void)AddProfile(profiles_, DefaultKeyProfileId, DefaultKeyProfileDisplayName);
}

KeyProfile* InputUserSettings::FindProfile(StringView id) noexcept {
    if (id.IsEmpty()) {
        return current_ < profiles_.Size() ? &profiles_[current_] : nullptr;
    }

    for (KeyProfile& profile : profiles_) {
        if (profile.id.View() == id) {
            return &profile;
        }
    }

    return nullptr;
}

const KeyProfile* InputUserSettings::FindProfile(StringView id) const noexcept {
    return const_cast<InputUserSettings*>(this)->FindProfile(id);
}

StringView InputUserSettings::CurrentProfileId() const noexcept {
    return current_ < profiles_.Size() ? profiles_[current_].id.View() : StringView();
}

Result<void> InputUserSettings::AddProfile(Array<KeyProfile>& profiles, StringView id,
                                           StringView displayName) const noexcept {
    KeyProfile profile(*allocator_);
    if (auto assigned = profile.id.TryAssign(id); !assigned) {
        return assigned;
    }

    if (auto assigned = profile.displayName.TryAssign(displayName); !assigned) {
        return assigned;
    }

    // 등록한 컨텍스트로 채운다(UE CreateNewKeyProfile :1563-1567).
    for (const InputContextBundle& bundle : registered_) {
        if (auto registered = RegisterToProfile(profile, bundle); !registered) {
            return registered;
        }
    }

    return profiles.TryPushBack(Move(profile));
}

Result<void> InputUserSettings::CopyProfiles(Array<KeyProfile>& out) const noexcept {
    // 생성자에서 기본 프로필을 못 만들었으면 사본에 다시 만든다(첫 변경이 고친다).
    if (profiles_.IsEmpty()) {
        return AddProfile(out, DefaultKeyProfileId, DefaultKeyProfileDisplayName);
    }

    if (auto reserved = out.TryReserve(profiles_.Size()); !reserved) {
        return reserved;
    }

    for (const KeyProfile& source : profiles_) {
        KeyProfile profile(*allocator_);
        if (auto assigned = profile.id.TryAssign(source.id.View()); !assigned) {
            return assigned;
        }

        if (auto assigned = profile.displayName.TryAssign(source.displayName.View()); !assigned) {
            return assigned;
        }

        if (auto reserved = profile.rows.TryReserve(source.rows.Size()); !reserved) {
            return reserved;
        }

        for (const KeyMappingRow& sourceRow : source.rows) {
            KeyMappingRow row(*allocator_);
            if (auto copied = SettingsCopyRow(row, sourceRow); !copied) {
                return copied;
            }

            (void)profile.rows.TryPushBack(Move(row));
        }

        (void)out.TryPushBack(Move(profile));
    }

    return Result<void>::Success();
}

Result<void> InputUserSettings::RegisterToProfile(KeyProfile& profile,
                                                  const InputContextBundle& bundle) const noexcept {
    // 이름마다 이번 등록에서 배정한 다음 slot이다(UE MappingsPerDeviceTypes — 장치 칸은 하나,
    // IAD9).
    Array<StringView> names(*allocator_);
    Array<uint8> nextSlots(*allocator_);
    for (const InputBinding& binding : SettingsProfileBindings(bundle, profile.id.View())) {
        const InputActionData* action = SettingsFindAction(bundle, binding.action);
        const PlayerMappingSettings* mapping =
            action != nullptr ? MappingSettingsOf(binding, action->settings) : nullptr;
        if (mapping == nullptr || mapping->name.IsEmpty() ||
            !MappingSupportsProfile(mapping, profile.id.View())) {
            continue;
        }

        const StringView name = mapping->name.View();
        usize nameIndex = names.Size();
        for (usize index = 0; index < names.Size(); ++index) {
            nameIndex = names[index] == name ? index : nameIndex;
        }

        if (nameIndex == names.Size()) {
            if (auto pushed = names.TryPushBack(name); !pushed) {
                return pushed;
            }

            if (auto pushed = nextSlots.TryPushBack(0); !pushed) {
                return pushed;
            }
        }

        const uint8 slot = nextSlots[nameIndex];
        // 칸을 다 쓰면 넘친 바인딩은 행에 넣지 않고 알린다(UE는 Unspecified·Max 칸에 넣는다 —
        // Rocks는 7칸만 둔다, 5.2절).
        if (slot >= KeySlotCount) {
            Log::Warning(LogInputActions,
                         "Input mapping '{Name}' has more than {Count} bindings in context "
                         "{Context}; the extra binding is not rebindable ({Code})",
                         name, KeySlotCount, bundle.id, DiagInputMappingSlotOverflow);
            continue;
        }

        nextSlots[nameIndex] = static_cast<uint8>(slot + 1);
        InputKey key;
        if (auto parsed = ParseInputKey(binding.key.View()); parsed) {
            key = parsed.Value();
        }

        KeyMappingRow* row = SettingsFindRow(profile, name);
        if (row == nullptr) {
            KeyMappingRow created(*allocator_);
            if (auto assigned = created.mappingName.TryAssign(name); !assigned) {
                return assigned;
            }

            if (auto pushed = profile.rows.TryPushBack(Move(created)); !pushed) {
                return pushed;
            }

            row = &profile.rows[profile.rows.Size() - 1];
        }

        // 같은 slot이 이미 있으면(불러온 사용자 키) 기본 키만 고친다. 기본 키와 같아졌으면 저장할
        // 것이 없으니 dirty를 지운다(UE :1736-1740·UpdateDefaultKeyFromActionKeyMapping :298-310).
        if (PlayerKeyMapping* existing = SettingsFindSlot(*row, slot)) {
            existing->defaultKey = key;
            existing->dirty = existing->dirty && existing->IsCustomized();
            continue;
        }

        PlayerKeyMapping created;
        created.slot = slot;
        created.defaultKey = key;
        created.currentKey = key;
        if (auto pushed = row->mappings.TryPushBack(created); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> InputUserSettings::RegisterContext(const InputContextBundle& bundle) noexcept {
    if (IsContextRegistered(bundle.id)) {
        return Result<void>::Success();
    }

    // 사본에 다 해 본 뒤 바꾼다(실패면 그대로).
    Allocator& allocator = *allocator_;
    InputContextBundle copy(allocator);
    copy.id = bundle.id;
    if (auto copied = detail::TryCopyValue(copy.data, bundle.data, allocator); !copied) {
        return copied;
    }

    if (auto copied = detail::TryCopyValue(copy.actions, bundle.actions, allocator); !copied) {
        return copied;
    }

    Array<KeyProfile> staged(allocator);
    if (auto copied = CopyProfiles(staged); !copied) {
        return copied;
    }

    // 프로필 override 이름마다 없는 프로필을 만든다(UE :1643-1660 — 현재 프로필은 그대로).
    for (const InputProfileBindings& entry : bundle.data.profileOverrides) {
        bool known = false;
        for (const KeyProfile& profile : staged) {
            known = known || profile.id.View() == entry.profile.View();
        }

        if (!known) {
            if (auto added = AddProfile(staged, entry.profile.View(), DefaultKeyProfileDisplayName);
                !added) {
                return added;
            }
        }
    }

    for (KeyProfile& profile : staged) {
        if (auto registered = RegisterToProfile(profile, bundle); !registered) {
            return registered;
        }
    }

    if (auto reserved = registered_.TryEnsureCapacity(registered_.Size() + 1); !reserved) {
        return reserved;
    }

    profiles_ = Move(staged);
    (void)registered_.TryPushBack(Move(copy));
    ++revision_;
    return Result<void>::Success();
}

Result<void> InputUserSettings::RefreshContext(const InputContextBundle& bundle) noexcept {
    for (InputContextBundle& registered : registered_) {
        if (registered.id != bundle.id) {
            continue;
        }

        // 사본에 다 만든 뒤 바꾼다(실패면 그대로).
        InputContextBundle copy(*allocator_);
        copy.id = bundle.id;
        if (auto copied = detail::TryCopyValue(copy.data, bundle.data, *allocator_); !copied) {
            return copied;
        }

        if (auto copied = detail::TryCopyValue(copy.actions, bundle.actions, *allocator_);
            !copied) {
            return copied;
        }

        registered = Move(copy);
        return Result<void>::Success();
    }

    return Result<void>::Success();
}

void InputUserSettings::UnregisterContext(const Guid& contextId) noexcept {
    for (usize index = 0; index < registered_.Size(); ++index) {
        if (registered_[index].id == contextId) {
            (void)registered_.TryRemoveAt(index);
            return;
        }
    }
}

bool InputUserSettings::IsContextRegistered(const Guid& contextId) const noexcept {
    for (const InputContextBundle& bundle : registered_) {
        if (bundle.id == contextId) {
            return true;
        }
    }

    return false;
}

Result<void> InputUserSettings::MapPlayerKey(StringView mappingName, uint8 slot, InputKey key,
                                             StringView profile, bool createSlotIfNeeded) noexcept {
    if (mappingName.IsEmpty() || !key.IsValid() || slot >= KeySlotCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    KeyProfile* target = FindProfile(profile);
    if (target == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    KeyMappingRow* row = SettingsFindRow(*target, mappingName);
    if (row == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (PlayerKeyMapping* mapping = SettingsFindSlot(*row, slot)) {
        // 같은 키면 바뀐 것이 없다(UE SetCurrentKey — 키가 다를 때만 dirty).
        if (mapping->currentKey == key) {
            return Result<void>::Success();
        }

        mapping->currentKey = key;
        mapping->dirty = true;
        ++revision_;
        return Result<void>::Success();
    }

    // 새 slot은 그 이름의 매핑이 하나 이상 있을 때만 만든다(기본 키 없음 — UE :1192-1230).
    if (!createSlotIfNeeded || row->mappings.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    PlayerKeyMapping created;
    created.slot = slot;
    created.currentKey = key;
    created.dirty = true;
    if (auto pushed = row->mappings.TryPushBack(created); !pushed) {
        return pushed;
    }

    ++revision_;
    return Result<void>::Success();
}

Result<void> InputUserSettings::UnmapPlayerKey(StringView mappingName, uint8 slot,
                                               StringView profile) noexcept {
    KeyProfile* target = FindProfile(profile);
    KeyMappingRow* row = target != nullptr ? SettingsFindRow(*target, mappingName) : nullptr;
    PlayerKeyMapping* mapping = row != nullptr ? SettingsFindSlot(*row, slot) : nullptr;
    if (mapping == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 이미 기본 키면 바뀐 것이 없다(UE :1280 — dirty일 때만 알린다).
    if (!mapping->IsCustomized()) {
        return Result<void>::Success();
    }

    mapping->currentKey = mapping->defaultKey;
    mapping->dirty = true;
    ++revision_;
    return Result<void>::Success();
}

Result<void> InputUserSettings::FindMappingsUsingKey(InputKey key,
                                                     Array<String>& out) const noexcept {
    out.Clear();
    const KeyProfile* profile = FindProfile({});
    if (profile == nullptr) {
        return Result<void>::Success();
    }

    for (const KeyMappingRow& row : profile->rows) {
        for (const PlayerKeyMapping& mapping : row.mappings) {
            if (mapping.currentKey == key) {
                String name(*allocator_);
                if (auto assigned = name.TryAssign(row.mappingName.View()); !assigned) {
                    return assigned;
                }

                if (auto pushed = out.TryPushBack(Move(name)); !pushed) {
                    return pushed;
                }

                break;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> InputUserSettings::KeysForBinding(StringView mappingName, InputKey defaultKey,
                                               Array<InputKey>& out) const noexcept {
    out.Clear();
    const KeyProfile* profile = FindProfile({});
    const KeyMappingRow* row =
        profile != nullptr ? SettingsFindRow(*profile, mappingName) : nullptr;
    if (row == nullptr) {
        return Result<void>::Success();
    }

    for (const PlayerKeyMapping& mapping : row->mappings) {
        const InputKey key = mapping.currentKey;
        // 기본 키와 같은 종류만(UE bMatchBasicKeyTypes — 기본 키가 무효면 거르지 않는다).
        const bool sameKind = !defaultKey.IsValid() || (key.IsGamepad() == defaultKey.IsGamepad() &&
                                                        key.IsTouch() == defaultKey.IsTouch());
        if (key.IsValid() && sameKind) {
            if (auto pushed = out.TryPushBack(key); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> InputUserSettings::CreateProfile(StringView id, StringView displayName) noexcept {
    if (id.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 생성자에서 기본 프로필을 못 만들었으면 먼저 만든다(복구 — 뒤 단계가 실패해도 남는다).
    if (profiles_.IsEmpty()) {
        if (auto added = AddProfile(profiles_, DefaultKeyProfileId, DefaultKeyProfileDisplayName);
            !added) {
            return added;
        }
    }

    for (const KeyProfile& profile : profiles_) {
        if (profile.id.View() == id) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (auto added = AddProfile(profiles_, id, displayName); !added) {
        return added;
    }

    ++revision_;
    return Result<void>::Success();
}

Result<void> InputUserSettings::SetCurrentProfile(StringView id) noexcept {
    for (usize index = 0; index < profiles_.Size(); ++index) {
        if (profiles_[index].id.View() == id) {
            if (current_ != index) {
                current_ = index;
                ++revision_;
            }

            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> InputUserSettings::ResetProfileToDefault(StringView id) noexcept {
    KeyProfile* profile = FindProfile(id);
    if (profile == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    bool changed = false;
    for (KeyMappingRow& row : profile->rows) {
        for (PlayerKeyMapping& mapping : row.mappings) {
            if (mapping.IsCustomized()) {
                mapping.currentKey = mapping.defaultKey;
                mapping.dirty = true;
                changed = true;
            }
        }
    }

    if (changed) {
        ++revision_;
    }

    return Result<void>::Success();
}

} // namespace rocks::input
