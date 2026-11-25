#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <InputActions/TriggerTypes.h>

namespace rocks::input {

// 트리거 평가 문맥이다. deltaSeconds는 dilation을 뺀 dt, timeDilation은 시간 트리거가
// affectedByTimeDilation일 때 곱한다(4.5절). chordStates는 액션 번호마다 이번 평가의 바인딩 수준
// 결합 상태다(3.2절). ChordAction·ChordBlocker는 chordStates[chordAction]을 읽고, 범위 밖이면
// None이다(UE 대상 instance data 없음과 같다).
struct TriggerContext {
    double deltaSeconds = 0.0;
    double timeDilation = 1.0;
    Span<const TriggerState> chordStates = {};
};

} // namespace rocks::input
