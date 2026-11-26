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
};

} // namespace rocks

ROCKS_DECLARE_HOST_SERVICE(rocks::AudioEngineHostOptions, ROCKS_ENGINEAUDIO_API)
