#pragma once

#include <Core/Text/StringView.h>
#include <InputActions/InputActionSettings.h>
#include <InputActions/InputBinding.h>
#include <InputActions/PlayerMappingSettings.h>

// 바인딩의 리바인딩 표시 정보 조회다(Docs/Design/InputActions 4.2절 2·5.2절 — 다시 만들기와 사용자
// 설정 등록이 같이 쓴다).
namespace rocks::input {

// 바인딩의 매핑 설정이다(UE FEnhancedActionKeyMapping::GetPlayerMappableKeySettings):
// InheritFromAction은 액션의 것, Override는 바인딩의 것, Ignore는 없음. 없으면 리바인딩 대상이
// 아니다(UE IsPlayerMappable).
inline const PlayerMappingSettings* MappingSettingsOf(const InputBinding& binding,
                                                      const InputActionSettings& action) noexcept {
    switch (binding.settingBehavior) {
    case MappingSettingBehavior::InheritFromAction:
        return action.mappingSettings.HasValue() ? &action.mappingSettings.Value() : nullptr;
    case MappingSettingBehavior::Override:
        return binding.mappingSettings.HasValue() ? &binding.mappingSettings.Value() : nullptr;
    case MappingSettingBehavior::Ignore:
        break;
    }

    return nullptr;
}

// 매핑 설정이 profile을 지원하는지다(supportedProfiles가 비면 모든 프로필 — UE :986-990·1695-1700).
inline bool MappingSupportsProfile(const PlayerMappingSettings* settings,
                                   StringView profile) noexcept {
    if (settings == nullptr || settings->supportedProfiles.IsEmpty()) {
        return true;
    }

    for (const String& supported : settings->supportedProfiles) {
        if (supported.View() == profile) {
            return true;
        }
    }

    return false;
}

} // namespace rocks::input
