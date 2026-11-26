#pragma once

#include <Base/Types.h>

// 제어 쪽(AudioDeviceContext) 계수다(Docs/Design/AudioSystem 3.35.6절 — 진단·시험). 제어 쪽이
// 묶음마다 사본을 결과 묶음에 실어 보내므로, 게임 스레드는 원자값 없이 그 사본을 읽는다.
namespace rocks {

struct AudioControlStats {
    // 지금 voice를 쥔 재생 수(놓기 대기 제외)와 놓기 대기 voice 수.
    uint32 activeVoices = 0;
    uint32 releasingVoices = 0;
    // 등록된 clip 수(해제 대기 포함).
    uint32 registeredClips = 0;
    // 명령 링이 차서 미룬 Play 수(누계)와 지금 미룬 수.
    uint64 deferredPlays = 0;
    uint32 pendingDeferred = 0;
    // voice 상한·clip 실패로 끝낸 재생 수(누계).
    uint64 voiceLimitRejects = 0;
    uint64 failedPlays = 0;
    // 보낸 마스터 gain 명령 수(누계)와 링이 차서 다음 묶음으로 미룬 수(누계).
    uint64 masterGainCommands = 0;
    uint64 masterGainRetries = 0;
};

} // namespace rocks
