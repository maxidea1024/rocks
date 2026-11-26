#pragma once

#include <Audio/AudioMixerTypes.h>
#include <Base/Types.h>
#include <Core/Function.h>

// 재생 handle·상태·결과·옵션이다(Docs/Design/AudioSystem 3.4·3.35.4절).
namespace rocks {

// 게임 쪽 재생 표의 용량이다(3.5.1절 — 차면 CapacityExceeded).
inline constexpr uint32 MaxPlaybacks = 1024;

// 게임이 시작한 재생 하나다. generation 0은 무효다. 슬롯은 Ended 결과를 전달한 뒤 재사용되며
// generation이 올라가므로, 이전 handle의 명령은 아무 일도 하지 않는다.
struct PlaybackHandle {
    uint32 slot = 0;
    uint32 generation = 0;

    bool IsValid() const noexcept {
        return generation != 0;
    }

    friend bool operator==(PlaybackHandle left, PlaybackHandle right) noexcept {
        return left.slot == right.slot && left.generation == right.generation;
    }
};

enum class PlaybackStatus : uint8 {
    // 수락했고 아직 시작 결과가 없다.
    Pending,
    // 시작 frame을 받았다.
    Playing,
    // 장치 없음·무음 실행이라 소리가 나지 않는다(다음 프레임에 Ended(Ignored)).
    Ignored,
    // terminal을 받았거나 이미 수거되어 handle이 낡았다.
    Finished
};

enum class PlaybackEndReason : uint8 {
    None,
    // clip 끝(loop가 아닌 재생).
    NaturalEnd,
    // Stop 호출.
    Stopped,
    // 소유 World가 사라졌다(맵 교체·Tryout Stop 포함).
    SceneRemoved,
    // 부착 emitter(AudioComponent)가 해제됐다.
    EmitterRemoved,
    // 실제 voice 상한(audio.voices.max)에 걸렸다(동시 발음·steal은 AU7).
    VoiceLimit,
    // 장치가 없다.
    Ignored,
    // 엔진 종료가 끊었다.
    ShuttingDown,
    // clip 등록 실패 등 시작할 수 없었다.
    Failed
};

enum class PlaybackEventKind : uint8 {
    Started,
    Ended
};

// 재생마다 Started 0–1번, Ended 정확히 1번 온다(3.5.4절). mixFrame은 Started면 시작 frame, Ended면
// 끝 frame이다(소리를 내지 않고 끝났으면 audio::UnsetMixFrame).
struct PlaybackEvent {
    PlaybackHandle handle;
    PlaybackEventKind kind = PlaybackEventKind::Started;
    PlaybackEndReason reason = PlaybackEndReason::None;
    uint64 mixFrame = audio::UnsetMixFrame;
};

using PlaybackEventCallback = Function<void(const PlaybackEvent&)>;

// 재생 옵션이다(AU3). 시작 시각 지정·동시 발음 재정의는 AU7·AU10.
struct PlayOptions {
    // 사운드 값에 곱하는 배수다. volume [0, MaxVoiceGain], pitch [MinPitch, MaxPitch](곱한 결과도
    // 같은 범위로 자른다).
    float volume = 1.0f;
    float pitch = 1.0f;
    // 시작 위치(원본 frame)다. clip frame 수 미만이어야 하고, loop면 loop 끝 미만이어야 한다.
    uint32 startFrame = 0;
    // 0에서 volume까지 올리는 길이(mix frame)다. 0이면 바로 volume이다. 제어 쪽이 묶음마다
    // 평가하므로 해상도는 게임 프레임 간격이다(3.35.6절).
    uint32 fadeInFrames = 0;
    // 결과 콜백이다(게임 스레드의 OnFrameBegin — 비어도 된다). 잡은 대상의 수명은 호출자 책임이다.
    // 엔진 종료 뒤에는 부르지 않는다.
    PlaybackEventCallback onEvent;
};

enum class PlaybackParameter : uint8 {
    Volume,
    Pitch
};

// 엔진 소유 재생(World 교체를 넘는 음악 등)과 World 소유 재생을 가른다.
enum class PlaybackOwnership : uint8 {
    World,
    Engine
};

// World 하나의 재생 공간이다. epoch는 슬롯 재사용을 가른다(0은 무효).
struct AudioSceneId {
    uint32 index = 0;
    uint32 epoch = 0;

    bool IsValid() const noexcept {
        return epoch != 0;
    }
};

// 부착 재생의 위치 원천(AudioComponent 하나)이다. generation 0은 무효다.
struct AudioEmitterId {
    uint32 slot = 0;
    uint32 generation = 0;

    bool IsValid() const noexcept {
        return generation != 0;
    }
};

} // namespace rocks
