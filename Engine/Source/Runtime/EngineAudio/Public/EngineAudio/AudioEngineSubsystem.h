#pragma once

#include <Audio/AudioOutputDevice.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/BoundedSpscQueue.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/TypeKey.h>
#include <Engine/Engine.h>
#include <EngineAudio/AudioEngineHostOptions.h>
#include <EngineAudio/AudioPlayback.h>
#include <EngineAudio/AudioRunTraits.h>
#include <EngineAudio/SoundSourceRef.h>

// 엔진에 오디오 장치를 붙이는 Subsystem이다(Docs/Design/AudioSystem 3.8·3.35절, UE FAudioDevice·
// FAudioDeviceManager의 엔진 쪽 대응). 장치(실 backend 또는 Null)와 제어 pipe(AudioControl lane)를
// 가지고, 게임 스레드의 재생 요청을 묶음으로 모아 프레임 끝에 제어 쪽에 넘기며, 결과를 다음 프레임
// 시작에 전달한다.
//
// 사용 예(게임 실행 파일):
//   const EngineSubsystemType subsystems[] = {AudioEngineSubsystemType()};
//   config.subsystems = subsystems;
//   ...
//   auto* audio = engine.GetSubsystem<AudioEngineSubsystem>();
//   auto played = audio->Play2D(clickSound);   // 엔진 소유 2D 재생
//
// 스레드: 공개 함수는 모두 게임 스레드에서 부른다.
namespace rocks {

class AudioDeviceContext;
struct ControlBatch;

// 진단·시험용 계수다.
struct AudioEngineStats {
    // 빈 묶음이 없어 보내지 못한 프레임 수(제어 쪽 밀림 — 명령은 다음 프레임에 함께 간다).
    uint64 deferredFrames = 0;
    // lane Post가 실패한 수(할당 실패 — 묶음은 다음 프레임에 다시 보낸다).
    uint64 postFailures = 0;
    // 보낸 묶음 수와 돌려받은 묶음 수.
    uint64 submittedBatches = 0;
    uint64 returnedBatches = 0;
    // 지금 살아 있는 재생 handle 수.
    uint32 livePlaybacks = 0;
};

class ROCKS_ENGINEAUDIO_API AudioEngineSubsystem final : public EngineSubsystem {
public:
    explicit AudioEngineSubsystem(Allocator& allocator) noexcept;
    AudioEngineSubsystem(const AudioEngineSubsystem&) = delete;
    AudioEngineSubsystem& operator=(const AudioEngineSubsystem&) = delete;
    ~AudioEngineSubsystem() override;

    // 실행 판정 → 장치 열기(실패는 Null) → lane 시작(3.35.9절). 실패: 할당 실패, lane 시작 실패.
    // 실패하면 만든 것을 되돌린다.
    Result<void> Initialize(Engine& engine) noexcept override;
    // 돌려받은 묶음의 결과를 전달하고 Ignored 재생을 끝낸다.
    void OnFrameBegin(Engine& engine) noexcept override;
    // 마스터 gain을 붙여 이번 프레임 묶음을 제어 쪽에 보낸다.
    void OnFrameEnd(Engine& engine) noexcept override;
    // 새 재생을 닫는다(ShuttingDown).
    void BeginShutdown(Engine& engine) noexcept override;
    // 제어 pipe drain → stop-all → 꼬리 대기(상한 ShutdownDrainTimeoutMs) → 장치 Close → 참조 해제.
    // 실패하지 않는다(3.35.9절 — 수명 장벽은 backend Stop과 AudioRT join이다).
    Result<void> CompleteInFlightWork(Engine& engine) noexcept override;
    // CompleteInFlightWork를 거치지 않았으면(엔진 초기화 실패 정리) 같은 drain을 먼저 한다.
    void Deinitialize(Engine& engine) noexcept override;

    // 엔진 소유 2D 재생이다(World 교체를 넘는다 — 세션 음악·UI). 오류: 빈 사운드·범위 밖
    // 옵션(InvalidArgument), 재생 표 가득(CapacityExceeded), BeginShutdown 뒤(ShuttingDown), 할당
    // 실패. 장치가 없으면 Ignored handle이다.
    Result<PlaybackHandle> Play2D(const SoundSourceRef& sound, PlayOptions options = {}) noexcept;

    // 재생을 멈춘다(fadeFrames 동안 줄인 뒤 Ended(Stopped)). 낡은 handle은 아무 일도 하지 않는다.
    // 오류: 무효 handle(generation 0)은 InvalidArgument.
    Result<void> Stop(PlaybackHandle handle,
                      uint32 fadeFrames = audio::DefaultStopFadeFrames) noexcept;

    // volume([0, MaxVoiceGain])·pitch([MinPitch, MaxPitch]) 배수를 rampFrames 동안 바꾼다. 낡은
    // handle은 아무 일도 하지 않는다. 오류: 무효 handle·범위 밖·비유한 값은 InvalidArgument.
    Result<void> SetParameter(PlaybackHandle handle, PlaybackParameter parameter, float value,
                              uint32 rampFrames = audio::DefaultParamRampFrames) noexcept;

    PlaybackStatus Status(PlaybackHandle handle) const noexcept;

    // 호스트가 창 포커스 사건에서 부른다(audio.unfocused.volume — 3.35.9절). 기본은 true다.
    void SetApplicationForeground(bool foreground) noexcept {
        foreground_ = foreground;
    }

    // 초기화 때 정한 실행 판정이다.
    const AudioRunTraits& RunTraits() const noexcept {
        return traits_;
    }

    // 장치다(audioOutput이 false면 nullptr). 제어 쪽 면은 부르지 않는다 — Info·계수·Null Pump·
    // RenderAvailable(Manual 방식 시험)만 쓴다.
    audio::AudioOutputDevice* Device() noexcept {
        return device_.Get();
    }

    AudioEngineStats Stats() const noexcept;

    // 진단·시험: 지금까지 보낸 묶음의 제어 쪽 처리가 끝날 때까지 기다린다(Thread 제어 방식 시험이
    // 시간 대기 없이 순서를 만든다 — DedicatedLane::Flush). Inline 방식·장치 없음은 할 일이 없다.
    void FlushControl() noexcept;

    // 제어 쪽 계수다(진단·시험). 제어 Inline 방식이거나 종료 drain 뒤에만 정확하다.
    struct ContextStats {
        uint32 activeVoices = 0;
        uint32 releasingVoices = 0;
        uint32 registeredClips = 0;
        uint32 pendingDeferred = 0;
        uint64 deferredPlays = 0;
        uint64 voiceLimitRejects = 0;
        uint64 failedPlays = 0;
        uint64 masterGainCommands = 0;
    };
    ContextStats ControlStats() const noexcept;

private:
    struct GamePlayback {
        uint32 generation = 1;
        bool live = false;
        PlaybackStatus status = PlaybackStatus::Finished;
        PlaybackEventCallback onEvent;
        // 현재 묶음 안의 SetParameter 명령 색인 + 1(0 = 없음)과 그 묶음 번호다(합치기 — 3.35.5절).
        uint64 parameterBatch = 0;
        uint32 parameterCommand[2] = {};
    };

    Result<void> OpenDevice(Engine& engine) noexcept;
    Result<void> StartControl() noexcept;
    Result<PlaybackHandle> Accept(const SoundSourceRef& sound, PlayOptions& options,
                                  uint32 scene) noexcept;
    Result<void> ValidateOptions(const SoundSourceRef& sound,
                                 const PlayOptions& options) const noexcept;
    GamePlayback* FindLive(PlaybackHandle handle) noexcept;
    const GamePlayback* FindLive(PlaybackHandle handle) const noexcept;
    void Deliver(const PlaybackEvent& event) noexcept;
    void FreeSlot(uint32 slot) noexcept;
    float MasterGainTarget(Engine& engine) const noexcept;
    void SubmitCurrent() noexcept;
    void ProcessBatch(ControlBatch* batch) noexcept;
    void Drain() noexcept;
    void ReleaseAll() noexcept;

    Allocator& allocator_;
    AudioRunTraits traits_;
    AudioEngineHostOptions options_;
    UniquePtr<audio::AudioOutputDevice> device_;
    UniquePtr<AudioDeviceContext> context_;
    // DedicatedLane은 Task 모듈(Private 의존)이라 불투명하게 둔다.
    struct Control;
    UniquePtr<Control> control_;
    Array<GamePlayback> playbacks_;
    Array<uint32> freeSlots_;
    Array<PlaybackHandle> ignored_;
    Array<PlaybackEvent> delivering_;
    Array<ControlBatch*> freeBatches_;
    ControlBatch* current_ = nullptr;
    BoundedSpscQueue<ControlBatch*> returned_;
    uint64 batchSerial_ = 1;
    bool foreground_ = true;
    bool accepting_ = false;
    bool drained_ = true;
    AudioEngineStats stats_;
};

// Engine 설정 목록에 넣을 생성 정보다(이름 "Audio").
ROCKS_ENGINEAUDIO_API EngineSubsystemType AudioEngineSubsystemType() noexcept;

} // namespace rocks

ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::AudioEngineSubsystem, ROCKS_ENGINEAUDIO_API)
