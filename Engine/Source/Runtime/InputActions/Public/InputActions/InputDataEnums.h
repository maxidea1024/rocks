#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 입력 액션 데이터의 enum이다(Docs/Design/InputActions 2.3·2.4절).
namespace rocks::input {

// 한 액션에 바인딩 여럿이 값을 낼 때 합치는 방식이다(UE EInputActionAccumulationBehavior).
ROCKS_ENUM()
enum class ActionAccumulation : uint8 {
    // 성분마다 절댓값이 큰 쪽(같으면 나중 값)이다.
    TakeHighestAbsoluteValue,
    // 성분 합이다.
    Cumulative
};

inline constexpr uint32 ActionAccumulationCount = 2;

// 바인딩의 리바인딩 표시 정보를 어디서 가져오는지다(UE EPlayerMappableKeySettingBehaviors).
ROCKS_ENUM()
enum class MappingSettingBehavior : uint8 {
    // 액션의 mappingSettings를 쓴다.
    InheritFromAction,
    // 바인딩의 mappingSettings를 쓴다.
    Override,
    // 리바인딩 대상이 아니다.
    Ignore
};

inline constexpr uint32 MappingSettingBehaviorCount = 3;

// 같은 컨텍스트를 여러 번 넣을 때의 처리다(UE EMappingContextRegistrationTrackingMode — 4.1절).
ROCKS_ENUM()
enum class ContextRegistrationTracking : uint8 {
    // 다시 넣으면 우선순위를 바꾸고 다시 만든다. 한 번 빼면 빠진다.
    Untracked,
    // 넣은 횟수를 센다. 횟수가 0이 될 때 빠진다.
    CountRegistrations
};

inline constexpr uint32 ContextRegistrationTrackingCount = 2;

} // namespace rocks::input
