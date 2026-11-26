#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 클립 임포트 명세의 이벤트 하나다(Docs/Design/MeshAnimationAssets 7.6.2절 5 — ClipEvent, 이름은
// 문자열). droppedProperties는 원본에서 버린 종류별 속성 수다(합이 0보다 크면
// EventPropertiesDropped 경고 — 7.3절).
namespace rocks {

struct ClipImportEvent {
    float time = 0.0f;
    float duration = 0.0f;
    int32 track = 0;
    StringView name{};
    StringView type{};
    float triggerChance = 1.0f;
    bool triggerOnDedicatedServer = true;
    bool triggerOnFollower = false;
    uint32 droppedProperties = 0;
};

} // namespace rocks
