#pragma once

#include <Base/Types.h>

// 제어 쪽(AudioDeviceContext) 계수다(Docs/Design/AudioSystem 3.35.6절 — 진단·시험). 제어 쪽이
// 묶음마다 사본을 결과 묶음에 실어 보내므로, 게임 스레드는 원자값 없이 그 사본을 읽는다.
namespace rocks {

struct AudioControlStats {
    // voice 상한에 세는 voice 수(정지 fade 중 제외 — 3.41.1절), 정지·steal fade 중이거나 재생에서
    // 떼어 낸 voice 수, 놓기 대기 voice 수.
    uint32 activeVoices = 0;
    uint32 stoppingVoices = 0;
    uint32 releasingVoices = 0;
    // 지금 voice 없이 살아 있는 재생 수(상한 밖 loop·remainActive, 무음 loop — 3.41.1절).
    uint32 voicelessPlaybacks = 0;
    // 등록된 clip 수(해제 대기 포함).
    uint32 registeredClips = 0;
    // 명령 링이 차서 미룬 Play 수(누계)와 지금 미룬 수.
    uint64 deferredPlays = 0;
    uint32 pendingDeferred = 0;
    // 상한 밖이라 시작하지 못한 새 one-shot 수, clip 실패로 끝낸 재생 수(누계).
    uint64 voiceLimitRejects = 0;
    uint64 failedPlays = 0;
    // 상한으로 voice를 빼앗은 수, 들리지 않아 끝낸 재생 수, 복귀 때 voice·슬롯을 얻지 못한 수,
    // 여유가 없어 정지 fade 중 voice를 짧게 끊은 수(누계 — 3.41.1절).
    uint64 voiceSteals = 0;
    uint64 inaudibleEnds = 0;
    uint64 resumeFailures = 0;
    uint64 headroomCuts = 0;
    // 동시 발음(3.41.2절): 그룹이 받지 않은 재생 수, 그룹이 빼앗은 재생 수(누계), 지금 쓰는 그룹
    // 수.
    uint64 concurrencyRejects = 0;
    uint64 concurrencySteals = 0;
    uint32 concurrencyGroups = 0;
    // 가상화(3.41.3절): 지금 가상 재생 수, 가상이 된 수·실현된 수(누계).
    uint32 virtualPlaybacks = 0;
    uint64 virtualizations = 0;
    uint64 realizations = 0;
    // 실현 때 동시 발음 그룹 표가 가득해 가상으로 남은 수(누계 — 다음 간격에 다시 본다)다.
    uint64 failedRealizations = 0;
    // 차폐 질의 요청 수(누계 — 3.42.7절)다.
    uint64 occlusionRequests = 0;
    // 믹스(3.43절): 지금 활성 믹스 수, 표가 차서 무시한 push·passive·base·override·분류 거리
    // 배수(누계).
    uint32 activeMixes = 0;
    uint64 mixRejects = 0;
    uint64 mixOverrideRejects = 0;
    uint64 categoryScaleRejects = 0;
    // 보낸 마스터 gain 명령 수(누계)와 링이 차서 다음 묶음으로 미룬 수(누계).
    uint64 masterGainCommands = 0;
    uint64 masterGainRetries = 0;
    // 지금 잡은 스트림 슬롯 수(회수 대기 포함)와 슬롯이 없어 VoiceLimit로 끝낸 재생 수(누계)다
    // (3.40.3절).
    uint32 activeStreams = 0;
    uint64 streamSlotRejects = 0;
};

} // namespace rocks
