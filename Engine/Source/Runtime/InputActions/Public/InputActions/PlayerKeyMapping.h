#pragma once

#include <Base/Types.h>
#include <InputActions/InputKey.h>

namespace rocks::input {

// 사용자 키 칸 수다(First…Seventh — UE EPlayerMappableKeySlot, Docs/Design/InputActions 5.2절).
inline constexpr uint8 KeySlotCount = 7;

// 매핑 이름 하나의 slot 하나에 든 사용자 키다(UE FPlayerKeyMapping). defaultKey는 컨텍스트 바인딩의
// 키(등록이 채운다 — 사용자가 새로 만든 slot은 무효 키), currentKey는 지금 쓰는 키다.
struct PlayerKeyMapping {
    uint8 slot = 0;
    InputKey defaultKey;
    InputKey currentKey;
    // 저장 뒤 바뀌었다(UE bIsDirty — 저장은 customized이거나 dirty인 매핑을 쓴다).
    bool dirty = false;

    bool IsCustomized() const noexcept {
        return !(currentKey == defaultKey);
    }
};

} // namespace rocks::input
