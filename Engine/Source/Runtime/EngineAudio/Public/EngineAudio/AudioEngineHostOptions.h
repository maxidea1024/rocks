#pragma once

#include <Audio/AudioOutputDevice.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Optional.h>
#include <EngineAudio/AudioRunTraits.h>

// 호스트가 고르는 오디오 엔진 실행 방식이다(Docs/Design/AudioSystem 3.35.9절). HostServices에
// 선택으로 등록한다. 없으면 실장치·스레드 기본값이다. 시험은 Inline 제어 + Manual render + Null
// Manual로 결정적으로 돈다.
//
// 사용 예(시험):
//   AudioEngineHostOptions options;
//   options.controlMode = AudioControlMode::Inline;
//   options.renderMode = audio::AudioRenderMode::Manual;
//   options.nullMode = audio::NullAudioOutputMode::Manual;
//   options.useBackend = false;
//   (void)modules.Services().Register(options);   // 엔진 Initialize 전, 엔진 종료 뒤 Unregister
namespace rocks {

// 제어 pipe를 어디서 돌릴지다.
enum class AudioControlMode : uint8 {
    // 전용 스레드(AudioControl).
    Thread,
    // 프레임 끝을 부른 스레드에서 바로(시험).
    Inline
};

struct AudioEngineHostOptions {
    AudioControlMode controlMode = AudioControlMode::Thread;
    audio::AudioRenderMode renderMode = audio::AudioRenderMode::Thread;
    audio::NullAudioOutputMode nullMode = audio::NullAudioOutputMode::Timer;
    // false면 backend registry를 보지 않고 Null로 연다.
    bool useBackend = true;
    // 있으면 실행 판정(CurrentAudioRunTraits) 대신 쓴다.
    Optional<AudioRunTraits> traits;
    // 시험: AudioRT 명령 링 칸 수다(2의 거듭제곱 — 0이면 기본). 링 포화(Deferred)를 재현할 때만
    // 줄인다.
    uint32 commandCapacity = 0;
    // 시험: 스트림 디코드 슬롯 수다(0이면 audio.stream.maxstreams·기본). 슬롯 부족(VoiceLimit)을
    // 재현할 때만 줄인다.
    uint32 streamSlots = 0;
    // 시험: 제어 쪽이 묶음을 처리하기 직전에 부른다(제어 스레드). 이벤트 gate로 제어 쪽을 멈춰 묶음
    // 부족(deferredFrames)을 재현한다. 보통은 nullptr이다.
    void (*beforeControlBatch)(void* context) noexcept = nullptr;
    void* beforeControlBatchContext = nullptr;
};

} // namespace rocks

ROCKS_DECLARE_HOST_SERVICE(rocks::AudioEngineHostOptions, ROCKS_ENGINEAUDIO_API)
