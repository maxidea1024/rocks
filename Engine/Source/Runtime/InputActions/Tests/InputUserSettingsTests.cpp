#include <Base/InitializerList.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <InputActions/InputUserSettings.h>
#include <InputActions/PlayerActionInput.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 사용자 키 설정 시험이다(Docs/Design/InputActions 5.2·5.3절 — IA4): 등록의 slot 배정·재등록·칸
// 한도, 키 바꾸기·되돌리기 오류, 새 slot, 충돌 조회, 프로필(override·supportedProfiles·새 프로필
// 채우기·초기화), 평가기 적용(같은 종류 거르기·키 여럿·Revision 다시 만들기·등록 옵션),
// 저장·불러오기 (왕복·건너뛰기·깨진 입력·version), 할당 실패.
using namespace rocks;
using namespace rocks::input;
namespace input_user_settings_tests_file {

namespace {

constexpr uint64 IdHigh = 0x55534554;
constexpr uint64 Jump = 1;
constexpr uint64 Fire = 2;
constexpr uint64 Walk = 3;
constexpr uint64 MaxAllocationAttempts = 1024;

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

InputKey K(input::Key key) noexcept {
    return InputKey::FromKey(key);
}

InputKey Pad(GamepadButton button) noexcept {
    return InputKey::FromGamepadButton(button);
}

// 시험용 bundle 만들기다.
struct Builder {
    explicit Builder(Allocator& allocator, uint64 context) noexcept
        : allocator(allocator), bundle(allocator) {
        bundle.id = Id(context);
    }

    InputActionData& Action(uint64 id, InputValueType type = InputValueType::Boolean) noexcept {
        InputActionData action(allocator);
        action.id = Id(id);
        action.settings.valueType = type;
        ok = ok && bundle.actions.TryPushBack(rocks::Move(action)).IsOk();
        return bundle.actions[bundle.actions.Size() - 1];
    }

    // 리바인딩 가능한 액션이다(매핑 이름 name, supported가 비면 모든 프로필).
    InputActionData& Mappable(uint64 id, const char* name,
                              std::initializer_list<const char*> supported = {}) noexcept {
        InputActionData& action = Action(id);
        action.settings.mappingSettings.Emplace(Settings(name, supported));
        return action;
    }

    PlayerMappingSettings Settings(const char* name,
                                   std::initializer_list<const char*> supported = {}) noexcept {
        PlayerMappingSettings settings(allocator);
        ok = ok && settings.name.TryAssign(name).IsOk();
        for (const char* profile : supported) {
            String text(allocator);
            ok = ok && text.TryAssign(profile).IsOk();
            ok = ok && settings.supportedProfiles.TryPushBack(rocks::Move(text)).IsOk();
        }

        return settings;
    }

    InputBinding& Bind(uint64 action, const char* key) noexcept {
        return BindInto(bundle.data.bindings, action, key);
    }

    InputBinding& BindInto(Array<InputBinding>& list, uint64 action, const char* key) noexcept {
        InputBinding binding(allocator);
        binding.action = Id(action);
        ok = ok && binding.key.TryAssign(key).IsOk();
        ok = ok && list.TryPushBack(rocks::Move(binding)).IsOk();
        return list[list.Size() - 1];
    }

    // profile override 목록을 하나 더한다.
    Array<InputBinding>& Override(const char* profile) noexcept {
        InputProfileBindings entry(allocator);
        ok = ok && entry.profile.TryAssign(profile).IsOk();
        ok = ok && bundle.data.profileOverrides.TryPushBack(rocks::Move(entry)).IsOk();
        return bundle.data.profileOverrides[bundle.data.profileOverrides.Size() - 1].bindings;
    }

    Allocator& allocator;
    InputContextBundle bundle;
    bool ok = true;
};

const KeyProfile* FindProfile(const InputUserSettings& settings, const char* id) noexcept {
    for (const KeyProfile& profile : settings.Profiles()) {
        if (profile.id.View() == StringView(id)) {
            return &profile;
        }
    }

    return nullptr;
}

const KeyMappingRow* FindRow(const KeyProfile* profile, const char* name) noexcept {
    if (profile == nullptr) {
        return nullptr;
    }

    for (const KeyMappingRow& row : profile->rows) {
        if (row.mappingName.View() == StringView(name)) {
            return &row;
        }
    }

    return nullptr;
}

const PlayerKeyMapping* FindSlot(const KeyMappingRow* row, uint8 slot) noexcept {
    if (row == nullptr) {
        return nullptr;
    }

    for (const PlayerKeyMapping& mapping : row->mappings) {
        if (mapping.slot == slot) {
            return &mapping;
        }
    }

    return nullptr;
}

// 현재 프로필의 (name, slot) 매핑이다.
const PlayerKeyMapping* Current(const InputUserSettings& settings, const char* name,
                                uint8 slot) noexcept {
    const KeyProfile* profile = nullptr;
    for (const KeyProfile& candidate : settings.Profiles()) {
        profile = candidate.id.View() == settings.CurrentProfileId() ? &candidate : profile;
    }

    return FindSlot(FindRow(profile, name), slot);
}

// 상태 전체를 글로 적는다(할당 실패가 아무것도 바꾸지 않았는지 비교한다).
bool Fingerprint(const InputUserSettings& settings, StringBuilderBase& out) noexcept {
    bool ok =
        out.TryAppendFormat("{}|{}|", settings.CurrentProfileId(), settings.Revision()).IsOk();
    for (const KeyProfile& profile : settings.Profiles()) {
        ok = ok &&
             out.TryAppendFormat("[{}:{}", profile.id.View(), profile.displayName.View()).IsOk();
        for (const KeyMappingRow& row : profile.rows) {
            ok = ok && out.TryAppendFormat("({}", row.mappingName.View()).IsOk();
            for (const PlayerKeyMapping& mapping : row.mappings) {
                ok = ok && out.TryAppendFormat(" {}:{}/{}:{}/{}:{}", mapping.slot,
                                               static_cast<uint32>(mapping.defaultKey.kind),
                                               mapping.defaultKey.code,
                                               static_cast<uint32>(mapping.currentKey.kind),
                                               mapping.currentKey.code, mapping.dirty ? 1 : 0)
                               .IsOk();
            }

            ok = ok && out.TryAppendFormat(")").IsOk();
        }

        ok = ok && out.TryAppendFormat("]").IsOk();
    }

    return ok;
}

bool SameState(const InputUserSettings& settings, StringView before) noexcept {
    SystemAllocator allocator;
    StringBuilder<512> now(allocator);
    const bool same = Fingerprint(settings, now) && now.View() == before;
    if (!same) {
        printf("  before: %.*s\n  now:    %.*s\n", static_cast<int>(before.ByteCount()),
               before.Data(), static_cast<int>(now.View().ByteCount()), now.View().Data());
    }

    return same;
}

// 다시 만든 바인딩 (액션, 키) 열이 기대와 같은지 본다.
struct Expect {
    uint64 action;
    InputKey key;
};

template <usize Count>
bool BindingsAre(const PlayerActionInput& input, const Expect (&expected)[Count]) noexcept {
    bool same = input.BindingCount() == Count;
    for (usize index = 0; same && index < Count; ++index) {
        const InputBindingView view = input.Binding(index);
        same = view.action == Id(expected[index].action) && view.key == expected[index].key;
    }

    if (!same) {
        printf("  bindings:");
        for (usize index = 0; index < input.BindingCount(); ++index) {
            const InputBindingView view = input.Binding(index);
            printf(" (%u,%u:%u)", static_cast<unsigned>(view.action.low),
                   static_cast<unsigned>(view.key.kind), static_cast<unsigned>(view.key.code));
        }

        printf("\n");
    }

    return same;
}

bool Apply(PlayerActionInput& input, const InputState& state) noexcept {
    return input.ApplyPendingChanges(state, 0).IsOk();
}

// 진단 코드 경고 수를 센다(건너뛰기·칸 넘침).
class SkipCounter final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        skipped += Contains(event.message, "(InputUserSettingsEntrySkipped)") ? 1 : 0;
        overflowed += Contains(event.message, "(InputMappingSlotOverflow)") ? 1 : 0;
        return Result<void>::Success();
    }

    uint32 skipped = 0;
    uint32 overflowed = 0;

private:
    static bool Contains(StringView text, const char* needle) noexcept {
        const usize bytes = strlen(needle);
        for (usize start = 0; start + bytes <= text.ByteCount(); ++start) {
            if (memcmp(text.Data() + start, needle, bytes) == 0) {
                return true;
            }
        }

        return false;
    }
};

// 시험 하나 동안 경고를 SkipCounter로 모은다.
struct LogCapture {
    LogCapture() noexcept : logs(logAllocator, Config()), installed(logs) {
        started = logs.Start().IsOk() && logs.AddSink(counter).IsOk();
    }

    static LogSystemConfig Config() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Debug;
        return config;
    }

    SkipCounter& Pump() noexcept {
        (void)logs.Pump();
        return counter;
    }

    SkipCounter counter;
    SystemAllocator logAllocator;
    LogSystem logs;
    LogInstallScope installed;
    bool started = false;
};

} // namespace

// 5.2절 등록: 리바인딩 가능한 바인딩마다 이름별 slot을 0부터 매긴다(액션 상속·바인딩 Override·
// Ignore·설정 없음). 같은 id 재등록은 아무 일도 없고, 칸을 넘친 바인딩은 경고하고 넣지 않는다.
TEST(InputUserSettings_RegisterAssignsSlots) {
    LogCapture capture;
    REQUIRE(capture.started);
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    REQUIRE(settings.Profiles().Size() == 1);
    CHECK(settings.CurrentProfileId() == StringView(DefaultKeyProfileId));
    CHECK(settings.Profiles()[0].displayName.View() == StringView(DefaultKeyProfileDisplayName));
    CHECK(settings.Revision() == 0);

    Builder builder(allocator, 10);
    builder.Mappable(Jump, "Jump");
    builder.Action(Fire);
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Jump, "W");
    builder.Bind(Jump, "Gamepad_FaceButton_Bottom");
    builder.Bind(Fire, "E");
    InputBinding& alt = builder.Bind(Fire, "Q");
    alt.settingBehavior = MappingSettingBehavior::Override;
    alt.mappingSettings.Emplace(builder.Settings("AltFire"));
    builder.Bind(Jump, "R").settingBehavior = MappingSettingBehavior::Ignore;
    REQUIRE(builder.ok);
    CHECK(!settings.IsContextRegistered(Id(10)));
    REQUIRE(settings.RegisterContext(builder.bundle));
    CHECK(settings.IsContextRegistered(Id(10)) && settings.Revision() == 1);
    const KeyProfile& profile = settings.Profiles()[0];
    REQUIRE(profile.rows.Size() == 2);
    const KeyMappingRow* jump = FindRow(&profile, "Jump");
    REQUIRE(jump != nullptr && jump->mappings.Size() == 3);
    const InputKey jumpKeys[] = {K(Key::Space), K(Key::W), Pad(GamepadButton::South)};
    for (uint8 slot = 0; slot < 3; ++slot) {
        const PlayerKeyMapping* mapping = FindSlot(jump, slot);
        REQUIRE(mapping != nullptr);
        CHECK(mapping->defaultKey == jumpKeys[slot] && mapping->currentKey == jumpKeys[slot]);
        CHECK(!mapping->IsCustomized() && !mapping->dirty);
    }

    const PlayerKeyMapping* altFire = FindSlot(FindRow(&profile, "AltFire"), 0);
    REQUIRE(altFire != nullptr);
    CHECK(altFire->defaultKey == K(Key::Q));

    // 같은 id 재등록은 아무 일도 없다.
    REQUIRE(settings.RegisterContext(builder.bundle));
    CHECK(settings.Revision() == 1 && settings.Profiles()[0].rows.Size() == 2);

    // 칸 한도: 여덟째 바인딩은 넣지 않는다(마지막 칸은 일곱째 키 그대로).
    Builder many(allocator, 11);
    many.Mappable(Walk, "Many");
    const char* const names[] = {"A", "B", "C", "D", "F", "G", "H", "I"};
    for (const char* name : names) {
        many.Bind(Walk, name);
    }

    REQUIRE(many.ok);
    REQUIRE(settings.RegisterContext(many.bundle));
    const KeyMappingRow* row = FindRow(&settings.Profiles()[0], "Many");
    REQUIRE(row != nullptr && row->mappings.Size() == KeySlotCount);
    const PlayerKeyMapping* last = FindSlot(row, KeySlotCount - 1);
    REQUIRE(last != nullptr);
    CHECK(last->currentKey == K(Key::H) && last->defaultKey == K(Key::H));
    CHECK(capture.Pump().overflowed == 1);

    // 해제는 목록에서만 뺀다(행은 남는다).
    settings.UnregisterContext(Id(11));
    settings.UnregisterContext(Id(99));
    CHECK(!settings.IsContextRegistered(Id(11)) && settings.IsContextRegistered(Id(10)));
    CHECK(FindRow(&settings.Profiles()[0], "Many") != nullptr);
}

// 5.2절 키: 바꾸기·되돌리기·새 slot·충돌 조회와 오류(모두 상태 그대로).
TEST(InputUserSettings_MapAndUnmapKeys) {
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    Builder builder(allocator, 20);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire");
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    REQUIRE(builder.ok);
    REQUIRE(settings.RegisterContext(builder.bundle));
    const uint64 registered = settings.Revision();

    CHECK(settings.MapPlayerKey("", 0, K(Key::Q)).Error() == ErrorCode::InvalidArgument);
    CHECK(settings.MapPlayerKey("Jump", 0, InputKey{}).Error() == ErrorCode::InvalidArgument);
    CHECK(settings.MapPlayerKey("Jump", KeySlotCount, K(Key::Q)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(settings.MapPlayerKey("Jump", 0, K(Key::Q), "NoProfile").Error() == ErrorCode::NotFound);
    CHECK(settings.MapPlayerKey("Crouch", 0, K(Key::Q)).Error() == ErrorCode::NotFound);
    CHECK(settings.MapPlayerKey("Jump", 1, K(Key::Q)).Error() == ErrorCode::NotFound);
    CHECK(settings.UnmapPlayerKey("Jump", 1).Error() == ErrorCode::NotFound);
    CHECK(settings.UnmapPlayerKey("Jump", 0, "NoProfile").Error() == ErrorCode::NotFound);
    CHECK(settings.Revision() == registered);

    // 같은 키로 바꾸기는 아무 일도 없다(dirty 아님).
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Space)));
    CHECK(settings.Revision() == registered && !Current(settings, "Jump", 0)->dirty);

    // 바꾸기: customized·dirty, Revision이 는다.
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Q)));
    const PlayerKeyMapping* jump = Current(settings, "Jump", 0);
    REQUIRE(jump != nullptr);
    CHECK(jump->currentKey == K(Key::Q) && jump->defaultKey == K(Key::Space));
    CHECK(jump->IsCustomized() && jump->dirty && settings.Revision() == registered + 1);

    // 충돌 조회: 현재 키가 같은 이름들(이름당 한 번).
    REQUIRE(settings.MapPlayerKey("Fire", 0, K(Key::Q)));
    Array<String> users(allocator);
    REQUIRE(settings.FindMappingsUsingKey(K(Key::Q), users));
    REQUIRE(users.Size() == 2);
    CHECK(users[0].View() == StringView("Jump") && users[1].View() == StringView("Fire"));
    REQUIRE(settings.FindMappingsUsingKey(K(Key::Space), users));
    CHECK(users.IsEmpty());

    // 새 slot: 옵션이 있어야 하고 기본 키는 무효다.
    REQUIRE(settings.MapPlayerKey("Jump", 2, K(Key::Z), {}, true));
    const PlayerKeyMapping* extra = Current(settings, "Jump", 2);
    REQUIRE(extra != nullptr);
    CHECK(!extra->defaultKey.IsValid() && extra->currentKey == K(Key::Z) && extra->dirty);

    // 되돌리기: 기본 키로, 이미 기본 키면 Revision 그대로.
    const uint64 beforeUnmap = settings.Revision();
    REQUIRE(settings.UnmapPlayerKey("Jump", 0));
    jump = Current(settings, "Jump", 0);
    REQUIRE(jump != nullptr);
    CHECK(jump->currentKey == K(Key::Space) && !jump->IsCustomized() && jump->dirty);
    CHECK(settings.Revision() == beforeUnmap + 1);
    REQUIRE(settings.UnmapPlayerKey("Jump", 0));
    CHECK(settings.Revision() == beforeUnmap + 1);
    // 사용자가 만든 slot을 되돌리면 무효 키(키 없음)가 된다.
    REQUIRE(settings.UnmapPlayerKey("Jump", 2));
    CHECK(!Current(settings, "Jump", 2)->currentKey.IsValid());
}

// 5.2절 프로필: override 이름마다 등록이 프로필을 만들고(현재 프로필 그대로), supportedProfiles가
// 거르고, 새 프로필은 등록한 컨텍스트로 채운다. 초기화·현재 프로필 바꾸기와 오류.
TEST(InputUserSettings_Profiles) {
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    Builder builder(allocator, 30);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire", {"InputUserSettings.Profiles.Default", "Classic"});
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    Array<InputBinding>& southpaw = builder.Override("Southpaw");
    builder.BindInto(southpaw, Jump, "Q");
    builder.BindInto(southpaw, Fire, "R");
    REQUIRE(builder.ok);
    REQUIRE(settings.RegisterContext(builder.bundle));
    REQUIRE(settings.Profiles().Size() == 2);
    CHECK(settings.CurrentProfileId() == StringView(DefaultKeyProfileId));
    const KeyProfile* south = FindProfile(settings, "Southpaw");
    REQUIRE(south != nullptr);
    // Southpaw는 override 목록이고, Fire는 Southpaw를 지원하지 않는다.
    CHECK(FindSlot(FindRow(south, "Jump"), 0)->defaultKey == K(Key::Q));
    CHECK(FindRow(south, "Fire") == nullptr);
    CHECK(FindSlot(FindRow(FindProfile(settings, DefaultKeyProfileId), "Fire"), 0) != nullptr);

    // 새 프로필은 등록한 컨텍스트로 채운다(override가 없으니 기본 bindings).
    CHECK(settings.CreateProfile("", "Empty").Error() == ErrorCode::InvalidArgument);
    CHECK(settings.CreateProfile("Southpaw", "Again").Error() == ErrorCode::AlreadyExists);
    REQUIRE(settings.CreateProfile("Classic", "Classic Keys"));
    const KeyProfile* classic = FindProfile(settings, "Classic");
    REQUIRE(classic != nullptr);
    CHECK(classic->displayName.View() == StringView("Classic Keys"));
    CHECK(FindSlot(FindRow(classic, "Jump"), 0)->defaultKey == K(Key::Space));
    CHECK(FindSlot(FindRow(classic, "Fire"), 0)->defaultKey == K(Key::E));

    // 다른 프로필의 키 바꾸기는 현재 프로필에 닿지 않는다.
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Z), "Classic"));
    CHECK(Current(settings, "Jump", 0)->currentKey == K(Key::Space));

    CHECK(settings.SetCurrentProfile("Nope").Error() == ErrorCode::NotFound);
    REQUIRE(settings.SetCurrentProfile("Classic"));
    const uint64 switched = settings.Revision();
    REQUIRE(settings.SetCurrentProfile("Classic"));
    CHECK(settings.Revision() == switched);
    CHECK(settings.CurrentProfileId() == StringView("Classic"));
    CHECK(Current(settings, "Jump", 0)->currentKey == K(Key::Z));

    // 초기화: 현재 프로필의 바꾼 키만, 바뀐 것이 없으면 Revision 그대로.
    CHECK(settings.ResetProfileToDefault("Nope").Error() == ErrorCode::NotFound);
    REQUIRE(settings.ResetProfileToDefault());
    CHECK(Current(settings, "Jump", 0)->currentKey == K(Key::Space));
    CHECK(Current(settings, "Jump", 0)->dirty && settings.Revision() == switched + 1);
    REQUIRE(settings.ResetProfileToDefault("Southpaw"));
    CHECK(settings.Revision() == switched + 1);
}

// 5.2절 적용: 붙은 평가기는 현재 프로필의 사용자 키로 다시 만든다(같은 종류만, slot이 여럿이면
// 바인딩도 여럿). 설정이 바뀌면 Revision으로 다시 만들고, notifyUserSettings 컨텍스트만
// 등록·해제한다.
TEST(PlayerActionInput_UserSettingsRebuild) {
    SystemAllocator allocator;
    InputState state;
    InputUserSettings settings(allocator);
    PlayerActionInput input(allocator);
    input.SetUserSettings(&settings);
    CHECK(input.UserSettings() == &settings && input.HasPendingRebuild());

    Builder builder(allocator, 40);
    builder.Mappable(Jump, "Jump");
    builder.Action(Fire);
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Jump, "Gamepad_FaceButton_Bottom");
    builder.Bind(Fire, "E");
    REQUIRE(builder.ok);
    ContextOptions notify;
    notify.notifyUserSettings = true;
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0, notify));
    CHECK(settings.IsContextRegistered(Id(40)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(
        input, {{Jump, K(Key::Space)}, {Jump, Pad(GamepadButton::South)}, {Fire, K(Key::E)}}));
    CHECK(!input.HasPendingRebuild());

    // 키 바꾸기 → Revision이 다시 만들게 한다. 게임패드 slot은 게임패드 바인딩에만 간다.
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Q)));
    REQUIRE(settings.MapPlayerKey("Jump", 1, Pad(GamepadButton::East)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input,
                      {{Jump, K(Key::Q)}, {Jump, Pad(GamepadButton::East)}, {Fire, K(Key::E)}}));

    // 같은 종류 slot이 둘이면 그 바인딩이 둘이 된다.
    REQUIRE(settings.MapPlayerKey("Jump", 2, K(Key::Z), {}, true));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::Q)},
                              {Jump, K(Key::Z)},
                              {Jump, Pad(GamepadButton::East)},
                              {Fire, K(Key::E)}}));

    // 바뀐 것이 없으면 다시 만들지 않는다.
    CHECK(!input.HasPendingRebuild());
    REQUIRE(Apply(input, state));

    // 현재 프로필 바꾸기도 다시 만든다(새 프로필은 기본 키 — 등록한 컨텍스트로 채웠다).
    REQUIRE(settings.CreateProfile("Fresh", "Fresh"));
    REQUIRE(settings.SetCurrentProfile("Fresh"));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(
        input, {{Jump, K(Key::Space)}, {Jump, Pad(GamepadButton::South)}, {Fire, K(Key::E)}}));

    // 옵션 없이 더한 컨텍스트는 등록하지 않는다. 옵션으로 빼면 해제한다.
    Builder plain(allocator, 41);
    plain.Mappable(Walk, "Walk");
    plain.Bind(Walk, "W");
    REQUIRE(plain.ok);
    REQUIRE(input.AddContext(rocks::Move(plain.bundle), 1));
    CHECK(!settings.IsContextRegistered(Id(41)));
    REQUIRE(input.RemoveContext(Id(40), notify));
    CHECK(!settings.IsContextRegistered(Id(40)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Walk, K(Key::W)}}));

    // 설정을 떼면 기본 키로 다시 만든다.
    REQUIRE(settings.CreateProfile("Other", "Other"));
    input.SetUserSettings(nullptr);
    CHECK(input.HasPendingRebuild());
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Walk, K(Key::W)}}));
}

// 5.3절 저장·불러오기: 바꾼(또는 dirty) 매핑만 쓰고 dirty를 지운다. 불러오면 프로필·현재 프로필·
// 키가 돌아오고, 그 뒤 등록이 기본 키를 채운다(사용자 키는 그대로).
TEST(InputUserSettings_SaveLoadRoundTrip) {
    SystemAllocator allocator;
    Builder builder(allocator, 50);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire");
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    REQUIRE(builder.ok);

    InputUserSettings settings(allocator);
    REQUIRE(settings.RegisterContext(builder.bundle));
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Q)));
    REQUIRE(settings.MapPlayerKey("Jump", 1, Pad(GamepadButton::North), {}, true));
    REQUIRE(settings.CreateProfile("Alt", "Alt \"Keys\""));
    REQUIRE(settings.MapPlayerKey("Fire", 0, K(Key::R), "Alt"));
    REQUIRE(settings.SetCurrentProfile("Alt"));

    StringBuilder<64> saved(allocator);
    REQUIRE(settings.Save(saved));
    // 사람이 고칠 수 있게 들여쓴 JSON이다(JsonWriter 기본).
    const StringView expected = R"({
  "version": 1,
  "currentProfile": "Alt",
  "profiles": [
    {
      "id": "InputUserSettings.Profiles.Default",
      "displayName": "Default Profile",
      "mappings": [
        {
          "name": "Jump",
          "slot": 0,
          "key": "Q"
        },
        {
          "name": "Jump",
          "slot": 1,
          "key": "Gamepad_FaceButton_Top"
        }
      ]
    },
    {
      "id": "Alt",
      "displayName": "Alt \"Keys\"",
      "mappings": [
        {
          "name": "Fire",
          "slot": 0,
          "key": "R"
        }
      ]
    }
  ]
}
)";
    CHECK(saved.View() == expected);
    if (saved.View() != expected) {
        printf("  saved: %.*s\n", static_cast<int>(saved.View().ByteCount()), saved.View().Data());
    }

    CHECK(!FindSlot(FindRow(FindProfile(settings, DefaultKeyProfileId), "Jump"), 0)->dirty);

    // dirty지만 기본 키로 돌아온 매핑도 한 번 쓴다(다음 불러오기가 예전 키를 덮게 — UE와 같다).
    REQUIRE(settings.UnmapPlayerKey("Fire", 0, "Alt"));
    saved.Clear();
    REQUIRE(settings.Save(saved));
    InputUserSettings reread(allocator);
    REQUIRE(reread.Load(saved.View(), allocator));
    const PlayerKeyMapping* wroteFire = FindSlot(FindRow(FindProfile(reread, "Alt"), "Fire"), 0);
    CHECK(wroteFire != nullptr && wroteFire->currentKey == K(Key::E));
    // 다시 쓰면(dirty 없음) 바꾸지 않은 매핑은 빠진다.
    StringBuilder<64> again(allocator);
    REQUIRE(settings.Save(again));
    CHECK(again.View().ByteCount() < saved.View().ByteCount());

    // 새 설정에 불러온다(등록 전): 프로필·현재 프로필·키, 기본 키는 아직 무효다.
    InputUserSettings loaded(allocator);
    REQUIRE(loaded.Load(expected, allocator));
    CHECK(loaded.CurrentProfileId() == StringView("Alt") && loaded.Revision() == 1);
    REQUIRE(loaded.Profiles().Size() == 2);
    CHECK(FindProfile(loaded, "Alt")->displayName.View() == StringView("Alt \"Keys\""));
    const PlayerKeyMapping* jump =
        FindSlot(FindRow(FindProfile(loaded, DefaultKeyProfileId), "Jump"), 0);
    REQUIRE(jump != nullptr);
    CHECK(jump->currentKey == K(Key::Q) && !jump->defaultKey.IsValid() && !jump->dirty);

    // 등록이 기본 키를 채운다(사용자 키 그대로, slot 1은 컨텍스트에 없어 기본 키 무효).
    REQUIRE(loaded.RegisterContext(builder.bundle));
    jump = FindSlot(FindRow(FindProfile(loaded, DefaultKeyProfileId), "Jump"), 0);
    CHECK(jump->defaultKey == K(Key::Space) && jump->currentKey == K(Key::Q));
    const PlayerKeyMapping* pad =
        FindSlot(FindRow(FindProfile(loaded, DefaultKeyProfileId), "Jump"), 1);
    CHECK(pad != nullptr && !pad->defaultKey.IsValid() &&
          pad->currentKey == Pad(GamepadButton::North));
    const PlayerKeyMapping* fire = FindSlot(FindRow(FindProfile(loaded, "Alt"), "Fire"), 0);
    CHECK(fire->defaultKey == K(Key::E) && fire->currentKey == K(Key::R));

    // 불러온 키가 기본 키와 같으면 등록 뒤 dirty가 아니다.
    InputUserSettings same(allocator);
    REQUIRE(same.MapPlayerKey("Jump", 0, K(Key::Space)).Error() == ErrorCode::NotFound);
    REQUIRE(same.Load("{\"version\":1,\"profiles\":[{\"id\":\"InputUserSettings.Profiles."
                      "Default\",\"mappings\":[{\"name\":\"Jump\",\"slot\":0,\"key\":\"SpaceBar\"}]"
                      "}]}",
                      allocator));
    CHECK(same.CurrentProfileId() == StringView(DefaultKeyProfileId));
    REQUIRE(same.MapPlayerKey("Jump", 0, K(Key::Space)));
    REQUIRE(same.RegisterContext(builder.bundle));
    const PlayerKeyMapping* restored = Current(same, "Jump", 0);
    CHECK(restored != nullptr && !restored->IsCustomized() && !restored->dirty);
}

// 5.3절 불러오기: 모르는 키 이름·범위 밖 slot은 경고와 함께 건너뛴다. 깨진 JSON·필수 칸 없음·종류
// 틀림은 InvalidArgument, 높은 version은 Unsupported이고 설정 그대로다. 현재 프로필 id가 없으면
// 기본 프로필이다.
TEST(InputUserSettings_LoadSkipsAndRejects) {
    LogCapture capture;
    REQUIRE(capture.started);
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    REQUIRE(settings.Load("{\"version\":1,\"currentProfile\":\"Gone\",\"profiles\":[{\"id\":"
                          "\"P\",\"mappings\":[{\"name\":\"Jump\",\"slot\":0,\"key\":\"NoKey\"},"
                          "{\"name\":\"Jump\",\"slot\":7,\"key\":\"E\"},{\"name\":\"\",\"slot\":0,"
                          "\"key\":\"E\"},{\"name\":\"Jump\",\"slot\":1,\"key\":\"e\"}]}]}",
                          allocator));
    CHECK(capture.Pump().skipped == 3);
    CHECK(settings.CurrentProfileId() == StringView(DefaultKeyProfileId));
    const KeyProfile* profile = FindProfile(settings, "P");
    REQUIRE(profile != nullptr && profile->rows.Size() == 1);
    CHECK(profile->displayName.View() == StringView(DefaultKeyProfileDisplayName));
    REQUIRE(profile->rows[0].mappings.Size() == 1);
    CHECK(profile->rows[0].mappings[0].slot == 1 &&
          profile->rows[0].mappings[0].currentKey == K(Key::E));

    SystemAllocator fingerprintAllocator;
    StringBuilder<512> before(fingerprintAllocator);
    REQUIRE(Fingerprint(settings, before));
    const char* const corrupt[] = {
        "",
        "{",
        "[]",
        "{\"profiles\":[]}",
        "{\"version\":\"1\",\"profiles\":[]}",
        "{\"version\":-1,\"profiles\":[]}",
        "{\"version\":1}",
        "{\"version\":1,\"profiles\":{}}",
        "{\"version\":1,\"profiles\":[1]}",
        "{\"version\":1,\"profiles\":[{\"mappings\":[]}]}",
        "{\"version\":1,\"profiles\":[{\"id\":\"\",\"mappings\":[]}]}",
        "{\"version\":1,\"profiles\":[{\"id\":\"Q\"}]}",
        "{\"version\":1,\"profiles\":[{\"id\":\"Q\",\"mappings\":[{\"name\":\"A\",\"slot\":0}]}]}",
        "{\"version\":1,\"profiles\":[{\"id\":\"Q\",\"mappings\":[{\"name\":\"A\",\"slot\":\"0\","
        "\"key\":\"E\"}]}]}",
        "{\"version\":1,\"profiles\":[{\"id\":\"Q\",\"mappings\":[{\"name\":1,\"slot\":0,\"key\":"
        "\"E\"}]}]}",
        // 앞 프로필이 멀쩡해도 뒤가 깨졌으면 아무것도 바꾸지 않는다.
        "{\"version\":1,\"profiles\":[{\"id\":\"Q\",\"mappings\":[]},{\"id\":3,\"mappings\":[]}]}",
    };
    for (const char* text : corrupt) {
        const auto loaded = settings.Load(text, allocator);
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        if (loaded || loaded.Error() != ErrorCode::InvalidArgument) {
            printf("  accepted: %s\n", text);
        }

        CHECK(SameState(settings, before.View()));
    }

    CHECK(settings.Load("{\"version\":2,\"profiles\":[]}", allocator).Error() ==
          ErrorCode::Unsupported);
    CHECK(SameState(settings, before.View()));
    // 낮은 version(0)은 받는다.
    REQUIRE(settings.Load("{\"version\":0,\"profiles\":[]}", allocator));
}

// 할당 실패: 등록·새 slot·프로필 만들기·불러오기·저장은 OutOfMemory이고 상태가 그대로다(저장 실패면
// dirty도 그대로).
TEST(InputUserSettings_AllocationFailuresKeepState) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    InputUserSettings settings(failing);
    Builder builder(allocator, 60);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire");
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Jump, "W");
    builder.Bind(Fire, "E");
    Array<InputBinding>& south = builder.Override("Southpaw");
    builder.BindInto(south, Jump, "Q");
    REQUIRE(builder.ok);
    Builder second(allocator, 61);
    second.Mappable(Walk, "Walk");
    second.Bind(Walk, "A");
    REQUIRE(second.ok);

    const char* const loadText =
        "{\"version\":1,\"currentProfile\":\"New\",\"profiles\":[{\"id\":\"New\",\"displayName\":"
        "\"A Long Display Name For Heap Storage\",\"mappings\":[{\"name\":\"Brand New Row Name\","
        "\"slot\":2,\"key\":\"F\"},{\"name\":\"Jump\",\"slot\":3,\"key\":\"G\"}]}]}";
    uint32 failures = 0;
    // 각 조작을 첫 할당부터 하나씩 실패시켜 본다(성공하면 다음 조작으로 간다).
    for (uint32 step = 0; step < 5; ++step) {
        bool done = false;
        for (uint64 attempt = 1; attempt < MaxAllocationAttempts && !done; ++attempt) {
            StringBuilder<512> before(allocator);
            REQUIRE(Fingerprint(settings, before));
            failing.FailOnAttempt(attempt);
            Result<void> result = Result<void>::Success();
            switch (step) {
            case 0:
                result = settings.RegisterContext(builder.bundle);
                break;
            case 1:
                result = settings.RegisterContext(second.bundle);
                break;
            case 2:
                result = settings.MapPlayerKey("Jump", 4, K(Key::Z), {}, true);
                break;
            case 3:
                result = settings.CreateProfile("A Profile Id Long Enough For Heap", "Display");
                break;
            default:
                result = settings.Load(loadText, allocator);
                break;
            }

            failing.DisableFailure();
            if (result) {
                done = true;
            } else {
                ++failures;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(SameState(settings, before.View()));
            }
        }

        CHECK(done);
    }

    CHECK(failures > 5);
    CHECK(settings.IsContextRegistered(Id(60)) && settings.IsContextRegistered(Id(61)));
    CHECK(settings.CurrentProfileId() == StringView("New"));
    CHECK(Current(settings, "Brand New Row Name", 2)->currentKey == K(Key::F));

    // 불러오기는 덮어쓰기라 사용자가 만든 slot 4가 없다(5.3절).
    REQUIRE(settings.SetCurrentProfile(DefaultKeyProfileId));
    CHECK(Current(settings, "Jump", 4) == nullptr);

    // 저장 실패: 출력 builder의 할당을 막는다(dirty 그대로).
    REQUIRE(settings.MapPlayerKey("Jump", 4, K(Key::Z), {}, true));
    REQUIRE(Current(settings, "Jump", 4) != nullptr && Current(settings, "Jump", 4)->dirty);
    test::NFailAllocator outputFailing(allocator);
    StringBuilder<1> output(outputFailing);
    outputFailing.FailOnAttempt(1);
    CHECK(settings.Save(output).Error() == ErrorCode::OutOfMemory);
    outputFailing.DisableFailure();
    CHECK(Current(settings, "Jump", 4)->dirty);
}

// 4.2절 2 겹침 합치기: 같은 이름의 같은 종류 바인딩 둘은 사용자 키 목록을 같이 받지만 (액션, 키,
// 종류 열)마다 한 항목이다(UE AddMapping Equals). 설정 없이 데이터가 겹쳐도 같다.
TEST(PlayerActionInput_UserKeysMergeDuplicateBindings) {
    SystemAllocator allocator;
    InputState state;
    InputUserSettings settings(allocator);
    PlayerActionInput input(allocator);
    input.SetUserSettings(&settings);
    Builder builder(allocator, 70);
    builder.Mappable(Walk, "Walk");
    builder.Action(Fire);
    builder.Bind(Walk, "W");
    builder.Bind(Walk, "Up");
    builder.Bind(Fire, "E");
    builder.Bind(Fire, "E");
    (void)builder.Bind(Fire, "E").triggers.TryPushBack(InputTrigger{});
    REQUIRE(builder.ok);
    ContextOptions notify;
    notify.notifyUserSettings = true;
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0, notify));
    REQUIRE(Apply(input, state));
    // W 바인딩은 (W, Up), Up 바인딩도 (W, Up)을 받는다 → 합쳐 둘. Fire E 둘은 하나, 트리거가 다른
    // E는 따로다.
    CHECK(BindingsAre(
        input, {{Walk, K(Key::W)}, {Walk, K(Key::Up)}, {Fire, K(Key::E)}, {Fire, K(Key::E)}}));
    REQUIRE(input.BindingCount() == 4);
    CHECK(input.Binding(2).triggers.Size() == 0 && input.Binding(3).triggers.Size() == 1);

    // slot 1을 W로 바꾸면 Walk는 W 하나다.
    REQUIRE(settings.MapPlayerKey("Walk", 1, K(Key::W)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Walk, K(Key::W)}, {Fire, K(Key::E)}, {Fire, K(Key::E)}}));
}

// 5.2절 적용: 설정의 현재 프로필이 활성 프로필이다(override 목록·supportedProfiles). 불러오기·
// 초기화도 Revision으로 다시 만들게 한다.
TEST(PlayerActionInput_UserSettingsProfileDrivesRebuild) {
    SystemAllocator allocator;
    InputState state;
    InputUserSettings settings(allocator);
    PlayerActionInput input(allocator);
    input.SetUserSettings(&settings);
    Builder builder(allocator, 71);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire", {DefaultKeyProfileId});
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    Array<InputBinding>& south = builder.Override("Southpaw");
    builder.BindInto(south, Jump, "Q");
    builder.BindInto(south, Fire, "R");
    REQUIRE(builder.ok);
    ContextOptions notify;
    notify.notifyUserSettings = true;
    REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0, notify));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::Space)}, {Fire, K(Key::E)}}));
    // Southpaw: override 목록이고 Fire는 Default 전용이라 빠진다. SetKeyProfile은 설정이 있으면
    // 쓰이지 않는다.
    REQUIRE(input.SetKeyProfile("Other"));
    REQUIRE(settings.SetCurrentProfile("Southpaw"));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::Q)}}));

    // 초기화·불러오기도 다시 만든다.
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::Z)));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::Z)}}));
    REQUIRE(settings.ResetProfileToDefault());
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::Q)}}));
    REQUIRE(settings.Load("{\"version\":1,\"currentProfile\":\"Southpaw\",\"profiles\":[{\"id\":"
                          "\"Southpaw\",\"mappings\":[{\"name\":\"Jump\",\"slot\":0,\"key\":\"X\"}"
                          "]}]}",
                          allocator));
    REQUIRE(Apply(input, state));
    CHECK(BindingsAre(input, {{Jump, K(Key::X)}}));
}

// 4.1절·5.2절 등록 옵션: CountRegistrations 컨텍스트는 마지막 제거에서만 해제한다. UpdateContext는
// 등록 사본을 바꿔 뒤에 만든 프로필이 새 데이터로 채워진다.
TEST(PlayerActionInput_UserSettingsRegistrationTracking) {
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    PlayerActionInput input(allocator);
    input.SetUserSettings(&settings);
    ContextOptions notify;
    notify.notifyUserSettings = true;
    for (uint32 round = 0; round < 2; ++round) {
        Builder builder(allocator, 72);
        builder.Mappable(Jump, "Jump");
        builder.Bind(Jump, "SpaceBar");
        builder.bundle.data.registrationTracking = ContextRegistrationTracking::CountRegistrations;
        REQUIRE(builder.ok);
        REQUIRE(input.AddContext(rocks::Move(builder.bundle), 0, notify));
    }

    REQUIRE(input.RemoveContext(Id(72), notify));
    CHECK(input.HasContext(Id(72)) && settings.IsContextRegistered(Id(72)));

    // 데이터 갱신: Jump 기본 키를 F로 → 새 프로필은 F다(기존 프로필 행은 그대로).
    Builder updated(allocator, 72);
    updated.Mappable(Jump, "Jump");
    updated.Bind(Jump, "F");
    updated.bundle.data.registrationTracking = ContextRegistrationTracking::CountRegistrations;
    REQUIRE(updated.ok);
    REQUIRE(input.UpdateContext(rocks::Move(updated.bundle), false));
    REQUIRE(settings.CreateProfile("Fresh", "Fresh"));
    CHECK(FindSlot(FindRow(FindProfile(settings, "Fresh"), "Jump"), 0)->defaultKey == K(Key::F));
    CHECK(Current(settings, "Jump", 0)->defaultKey == K(Key::Space));

    REQUIRE(input.RemoveContext(Id(72), notify));
    CHECK(!input.HasContext(Id(72)) && !settings.IsContextRegistered(Id(72)));
}

// 4.1절 원자성: notifyUserSettings 추가에서 평가기 칸 할당이 실패하면 설정에도 등록되지 않는다.
// 설정 쪽 할당 실패도 평가기가 그대로다.
TEST(PlayerActionInput_UserSettingsAddContextAllocationFailure) {
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    test::NFailAllocator failing(allocator);
    PlayerActionInput input(failing);
    input.SetUserSettings(&settings);
    ContextOptions notify;
    notify.notifyUserSettings = true;
    Builder builder(allocator, 73);
    builder.Mappable(Jump, "Jump");
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    const uint64 revision = settings.Revision();
    failing.FailOnAttempt(1);
    CHECK(input.AddContext(rocks::Move(builder.bundle), 0, notify).Error() ==
          ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(!input.HasContext(Id(73)) && !settings.IsContextRegistered(Id(73)));
    CHECK(settings.Revision() == revision && settings.Profiles()[0].rows.IsEmpty());

    // 설정 쪽 실패: 설정 allocator를 막는다.
    test::NFailAllocator settingsFailing(allocator);
    InputUserSettings strict(settingsFailing);
    PlayerActionInput other(allocator);
    other.SetUserSettings(&strict);
    settingsFailing.FailOnAttempt(1);
    CHECK(other.AddContext(rocks::Move(builder.bundle), 0, notify).Error() ==
          ErrorCode::OutOfMemory);
    settingsFailing.DisableFailure();
    CHECK(!other.HasContext(Id(73)) && !strict.IsContextRegistered(Id(73)));
    REQUIRE(other.AddContext(rocks::Move(builder.bundle), 0, notify));
    CHECK(strict.IsContextRegistered(Id(73)));
}

// 5.3절 덮어쓰기: 불러오기는 바꾼 키를 기본으로 되돌리고 사용자가 만든 slot을 지운 뒤 파일을
// 적용한다("저장본으로 되돌리기"). 비운 slot은 빈 키 이름으로 왕복한다(경고 없음).
TEST(InputUserSettings_LoadOverwritesAndClearedSlots) {
    LogCapture capture;
    REQUIRE(capture.started);
    SystemAllocator allocator;
    InputUserSettings settings(allocator);
    Builder builder(allocator, 74);
    builder.Mappable(Jump, "Jump");
    builder.Mappable(Fire, "Fire");
    builder.Bind(Jump, "SpaceBar");
    builder.Bind(Fire, "E");
    REQUIRE(builder.ok);
    REQUIRE(settings.RegisterContext(builder.bundle));
    REQUIRE(settings.MapPlayerKey("Fire", 0, K(Key::R)));
    StringBuilder<64> saved(allocator);
    REQUIRE(settings.Save(saved));

    // 저장 뒤 바꾼 것: Jump → E, Jump slot 1 새로 만듦, Fire → T.
    REQUIRE(settings.MapPlayerKey("Jump", 0, K(Key::E)));
    REQUIRE(settings.MapPlayerKey("Jump", 1, K(Key::G), {}, true));
    REQUIRE(settings.MapPlayerKey("Fire", 0, K(Key::T)));
    REQUIRE(settings.Load(saved.View(), allocator));
    CHECK(Current(settings, "Jump", 0)->currentKey == K(Key::Space));
    CHECK(Current(settings, "Jump", 1) == nullptr);
    CHECK(Current(settings, "Fire", 0)->currentKey == K(Key::R));
    CHECK(!Current(settings, "Jump", 0)->dirty && !Current(settings, "Fire", 0)->dirty);

    // 비운 slot: 만든 slot을 되돌리면 무효 키이고 dirty다 → "" 로 쓰고 다시 읽는다.
    REQUIRE(settings.MapPlayerKey("Jump", 2, K(Key::G), {}, true));
    StringBuilder<64> withSlot(allocator);
    REQUIRE(settings.Save(withSlot));
    REQUIRE(settings.UnmapPlayerKey("Jump", 2));
    StringBuilder<64> cleared(allocator);
    REQUIRE(settings.Save(cleared));
    InputUserSettings reread(allocator);
    REQUIRE(reread.RegisterContext(builder.bundle));
    REQUIRE(reread.Load(cleared.View(), allocator));
    const PlayerKeyMapping* empty = Current(reread, "Jump", 2);
    REQUIRE(empty != nullptr);
    CHECK(!empty->currentKey.IsValid() && !empty->defaultKey.IsValid());
    CHECK(capture.Pump().skipped == 0);
    // 비운 slot은 키를 내지 않는다.
    Array<InputKey> keys(allocator);
    REQUIRE(reread.KeysForBinding("Jump", K(Key::Space), keys));
    REQUIRE(keys.Size() == 1);
    CHECK(keys[0] == K(Key::Space));
}

// 생성자 할당 실패: 프로필이 없고 키 바꾸기는 NotFound다. 첫 등록·프로필 만들기·불러오기가 기본
// 프로필을 다시 만든다. 불러오기 문서 할당 실패는 OutOfMemory이고 그대로다.
TEST(InputUserSettings_RecoversDefaultProfileAndParseFailure) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    Builder builder(allocator, 75);
    builder.Mappable(Jump, "Jump");
    builder.Bind(Jump, "SpaceBar");
    REQUIRE(builder.ok);
    for (uint32 path = 0; path < 3; ++path) {
        failing.FailOnAttempt(1);
        InputUserSettings settings(failing);
        failing.DisableFailure();
        REQUIRE(settings.Profiles().IsEmpty());
        CHECK(settings.CurrentProfileId().IsEmpty());
        CHECK(settings.MapPlayerKey("Jump", 0, K(Key::E)).Error() == ErrorCode::NotFound);
        if (path == 0) {
            REQUIRE(settings.RegisterContext(builder.bundle));
        } else if (path == 1) {
            REQUIRE(settings.CreateProfile("Custom", "Custom"));
        } else {
            REQUIRE(settings.Load("{\"version\":1,\"profiles\":[]}", allocator));
        }

        CHECK(FindProfile(settings, DefaultKeyProfileId) != nullptr);
        CHECK(settings.CurrentProfileId() == StringView(DefaultKeyProfileId));
    }

    InputUserSettings settings(allocator);
    REQUIRE(settings.RegisterContext(builder.bundle));
    StringBuilder<512> before(allocator);
    REQUIRE(Fingerprint(settings, before));
    const char* const text = "{\"version\":1,\"profiles\":[{\"id\":\"A Long Profile Identifier\","
                             "\"mappings\":[{\"name\":\"Jump\",\"slot\":0,\"key\":\"E\"}]}]}";
    test::NFailAllocator scratch(allocator);
    uint32 failures = 0;
    bool loaded = false;
    for (uint64 attempt = 1; attempt < MaxAllocationAttempts && !loaded; ++attempt) {
        scratch.FailOnAttempt(attempt);
        const auto result = settings.Load(text, scratch);
        scratch.DisableFailure();
        if (result) {
            loaded = true;
        } else {
            ++failures;
            CHECK(result.Error() == ErrorCode::OutOfMemory);
            CHECK(SameState(settings, before.View()));
        }
    }

    CHECK(loaded && failures > 0);
}

} // namespace input_user_settings_tests_file
