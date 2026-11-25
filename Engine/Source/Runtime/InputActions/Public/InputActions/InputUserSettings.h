#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <InputActions/InputContextBundle.h>
#include <InputActions/InputKey.h>
#include <InputActions/KeyProfile.h>

namespace rocks::input {

// 기본 키 프로필 id다(UE DefaultProfileIdentifierString과 같은 문자열 — UE 컨텍스트의 프로필 이름과
// 맞는다).
inline constexpr char DefaultKeyProfileId[] = "InputUserSettings.Profiles.Default";
inline constexpr char DefaultKeyProfileDisplayName[] = "Default Profile";
// 저장 형식 버전이다(Docs/Design/InputActions 5.3절).
inline constexpr uint32 InputUserSettingsVersion = 1;

// 로컬 플레이어 하나의 키 리바인딩 설정이다(UE UEnhancedInputUserSettings —
// Docs/Design/InputActions 5.2·5.3절). 컨텍스트를 등록하면 리바인딩 가능한 바인딩마다 (이름, slot)
// 행을 만들고, 사용자가 키를 바꾸면 Revision이 늘어 붙은 평가기가 다시 만든다.
//
// 사용 예:
//   InputUserSettings settings(allocator);
//   playerInput.SetUserSettings(&settings);
//   (void)playerInput.AddContext(Move(bundle), 0, {.notifyUserSettings = true});
//   (void)settings.MapPlayerKey("Jump", 0, InputKey::FromKey(Key::E));
//
// 할당: 등록·키 바꾸기·프로필·불러오기 때 한다. 스레드: 게임 스레드 전용이다.
class ROCKS_INPUTACTIONS_API InputUserSettings {
public:
    // 기본 프로필 하나를 현재 프로필로 둔다(할당 실패면 프로필이 없다 — 키 바꾸기는 NotFound이고,
    // 첫 등록·프로필 만들기·불러오기가 기본 프로필을 다시 만든다).
    explicit InputUserSettings(Allocator& allocator) noexcept;
    InputUserSettings(const InputUserSettings&) = delete;
    InputUserSettings& operator=(const InputUserSettings&) = delete;

    // ---- 등록(5.2절) ----

    // bundle의 리바인딩 가능한 바인딩을 모든 프로필에 행으로 넣는다(프로필 override 이름마다 없는
    // 프로필을 만든다). 이미 등록한 id면 아무 일도 없다(UE와 같다). 사본을 둬서 나중에 만든
    // 프로필도 채운다. 할당 실패는 OutOfMemory(그대로)다.
    Result<void> RegisterContext(const InputContextBundle& bundle) noexcept;

    // 등록한 컨텍스트의 사본을 bundle로 바꾼다(PlayerActionInput::UpdateContext가 부른다 — 뒤에
    // 만드는 프로필이 새 데이터로 채워진다). 행은 다시 매기지 않는다. 등록하지 않은 id면 아무 일도
    // 없다. 할당 실패는 OutOfMemory(그대로)다.
    Result<void> RefreshContext(const InputContextBundle& bundle) noexcept;

    // 등록 목록에서 뺀다(행은 남는다 — UE와 같다). 없으면 아무 일도 없다.
    void UnregisterContext(const Guid& contextId) noexcept;

    bool IsContextRegistered(const Guid& contextId) const noexcept;

    // ---- 키(5.2절) ----

    // (mappingName, slot)의 현재 키를 key로 바꾼다(같은 키면 아무 일도 없다). profile이 비면 현재
    // 프로필이다.
    // createSlotIfNeeded면 slot이 없을 때 새로 만든다(그 이름의 매핑이 하나 이상 있어야 한다 — 기본
    // 키 없음). 오류: 빈 이름·무효 키·범위 밖 slot은 InvalidArgument, 없는 프로필·이름·slot은
    // NotFound, 할당 실패는 OutOfMemory(모두 그대로).
    Result<void> MapPlayerKey(StringView mappingName, uint8 slot, InputKey key,
                              StringView profile = {}, bool createSlotIfNeeded = false) noexcept;

    // (mappingName, slot)을 기본 키로 되돌린다. 없으면 NotFound다.
    Result<void> UnmapPlayerKey(StringView mappingName, uint8 slot,
                                StringView profile = {}) noexcept;

    // 현재 프로필에서 key를 쓰는 매핑 이름을 out에 넣는다(충돌 안내 — UE GetMappingNamesForKey).
    // 할당 실패는 OutOfMemory다.
    Result<void> FindMappingsUsingKey(InputKey key, Array<String>& out) const noexcept;

    // 다시 만들기가 쓴다: 현재 프로필의 mappingName 행에서 defaultKey와 같은 종류(게임패드·터치)인
    // 유효한 현재 키들을 out에 넣는다(5.2절 적용). 없으면 out은 비고 바인딩의 기본 키를 쓴다.
    Result<void> KeysForBinding(StringView mappingName, InputKey defaultKey,
                                Array<InputKey>& out) const noexcept;

    // ---- 프로필(5.2절) ----

    // 프로필을 만들고 등록한 컨텍스트로 채운다(같은 id는 AlreadyExists, 빈 id는 InvalidArgument,
    // 할당 실패는 OutOfMemory — 그대로).
    Result<void> CreateProfile(StringView id, StringView displayName) noexcept;

    // 현재 프로필을 바꾼다(없으면 NotFound).
    Result<void> SetCurrentProfile(StringView id) noexcept;

    // 프로필(빈 값 = 현재)의 모든 매핑을 기본 키로 되돌린다(없으면 NotFound).
    Result<void> ResetProfileToDefault(StringView id = {}) noexcept;

    StringView CurrentProfileId() const noexcept;
    Span<const KeyProfile> Profiles() const noexcept {
        return profiles_.View();
    }

    // 바뀔 때마다 는다(평가기가 다시 만들기를 판단한다).
    uint64 Revision() const noexcept {
        return revision_;
    }

    // ---- 저장(5.3절) ----

    // customized이거나 dirty인 매핑을 out에 JSON으로 쓴다(성공하면 dirty를 지운다). 오류: out의
    // 오류(OutOfMemory 등 — dirty는 그대로).
    Result<void> Save(StringBuilderBase& out) noexcept;

    // text를 읽어 매핑·현재 프로필을 덮어쓴다: 모든 매핑을 기본 키로 되돌리고 사용자가 만든 slot을
    // 지운 뒤 파일의 매핑을 넣는다(파일에 없는 프로필은 남는다). 빈 키 이름은 비운 slot이다. 모르는
    // 키 이름·범위 밖 slot은 건너뛰고 경고한다. 오류: 깨진 JSON·필수 칸 없음·종류 틀림은
    // InvalidArgument, 높은 version은 Unsupported, 할당 실패는 OutOfMemory(모두 설정 그대로).
    // scratch는 읽는 동안의 문서 메모리다.
    Result<void> Load(StringView text, Allocator& scratch) noexcept;

private:
    KeyProfile* FindProfile(StringView id) noexcept;
    const KeyProfile* FindProfile(StringView id) const noexcept;
    Result<void> RegisterToProfile(KeyProfile& profile,
                                   const InputContextBundle& bundle) const noexcept;
    Result<void> CopyProfiles(Array<KeyProfile>& out) const noexcept;
    Result<void> AddProfile(Array<KeyProfile>& profiles, StringView id,
                            StringView displayName) const noexcept;

    Allocator* allocator_;
    Array<KeyProfile> profiles_;
    usize current_ = 0;
    // 등록한 컨텍스트 사본이다(새 프로필을 채운다 — UE CreateNewKeyProfile :1563-1567).
    Array<InputContextBundle> registered_;
    uint64 revision_ = 0;
};

} // namespace rocks::input
