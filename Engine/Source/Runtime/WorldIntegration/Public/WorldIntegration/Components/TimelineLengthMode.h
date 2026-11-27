#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

namespace rocks {

// 타임라인 길이를 정하는 방식이다(UE ETimelineLengthMode — Docs/Design/EngineRuntime 32.2절).
ROCKS_ENUM()
enum class TimelineLengthMode : uint8 {
    // length 필드가 길이다(UE TL_TimelineLength).
    TimelineLength = 0,
    // 이벤트 키와 트랙 커브의 마지막 키 시간이 길이다(UE TL_LastKeyFrame).
    LastKeyFrame = 1,
};

} // namespace rocks
