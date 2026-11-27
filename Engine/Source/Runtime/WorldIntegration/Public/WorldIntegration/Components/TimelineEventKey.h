#pragma once

#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

namespace rocks {

// 타임라인 이벤트 키다(UE FTimelineEventEntry — Docs/Design/EngineRuntime 32.2절). 재생이 time을
// 지나가면 TimelineComponent::eventKeyReached(eventName)을 발행한다(UE EventFunc 대신 이름).
ROCKS_STRUCT()
struct TimelineEventKey {
    ROCKS_GENERATED_BODY()

    // 키 시간(초)이다.
    ROCKS_PROPERTY() float time = 0.0f;
    // 이벤트 이름이다(None도 된다 — 받는 쪽이 고른다).
    ROCKS_PROPERTY() Name eventName;
};

} // namespace rocks
