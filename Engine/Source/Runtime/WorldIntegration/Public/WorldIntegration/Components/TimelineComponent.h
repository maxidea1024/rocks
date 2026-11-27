#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>
#include <Object/Event.h>
#include <World/Component.h>
#include <WorldIntegration/Components/TimelineColorTrack.h>
#include <WorldIntegration/Components/TimelineEventKey.h>
#include <WorldIntegration/Components/TimelineFloatTrack.h>
#include <WorldIntegration/Components/TimelineLengthMode.h>
#include <WorldIntegration/Components/TimelineVectorTrack.h>

namespace rocks {

// 시간에 따라 커브 트랙을 재생하는 컴포넌트다(UE UTimelineComponent + FTimeline — Docs/Design/
// EngineRuntime 32장). 재생 위치를 Tick마다 delta × playRate만큼 움직이고, 트랙 값·이벤트 키·갱신·
// 끝을 이벤트로 알린다. 렌더·물리 없이 끝까지 동작한다.
//
// - Tick: PrePhysics, 시작 때 꺼져 있다. Play·PlayFromStart·Reverse·ReverseFromEnd가 켜고, 재생이
//   끝나 멈추면 Tick이 끈다(UE Activate·Deactivate — 컴포넌트 활성은 건드리지 않는다).
// - 트랙 커브: 인라인 값, 또는 SoftObjectRef 에셋의 로드 사본(ApplyLoaded*Curve — 로드 시스템이
//   부른다, 사본 전에는 그 트랙을 평가하지 않는다 — ERD71).
// - 이벤트(ERD72): 트랙 출력은 트랙 이름 인자의 컴포넌트 이벤트 하나다. 구독이 없는 종류의 트랙은
//   평가하지 않는다(결과 동작은 같다 — UE도 delegate가 없으면 평가하지 않는다).
// - 시간은 float(UE와 같다). 비유한 delta·playRate의 Tick은 건너뛴다(32.6절 3).
//
// 사용 예:
//   auto timeline = world.AddComponent<TimelineComponent>(door);
//   world.With<TimelineComponent>(timeline.Value(), [&](TimelineComponent& value) noexcept {
//       TangentCurve open(allocator);
//       (void)open.TryAddKey(0.0f, 0.0f);
//       (void)open.TryAddKey(1.0f, 90.0f);
//       (void)value.TryAddFloatTrack(angle, open);
//       (void)value.floatTrackUpdated.TryAddLambda([](Name, float degrees) noexcept { ... });
//       value.PlayFromStart();
//       return true;
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLDINTEGRATION_API TimelineComponent : public Component {
    ROCKS_GENERATED_BODY()

public:
    // UE FTimeline 생성자(TimelineComponent.h:274–286)의 길이다.
    static constexpr float DefaultLength = 5.0f;
    // 이벤트 키 구간을 끝·시작에서 넓히는 양이다(UE UE_KINDA_SMALL_NUMBER —
    // Timeline.cpp:433·445·647).
    static constexpr float EventEdgeEpsilon = 1.0e-4f;
    // loop에서 남은 위치를 뺄셈으로 줄이는 반복 상한이다(넘으면 정확한 나머지 — 32.6절 3).
    static constexpr uint32 MaxTimelineLoopWraps = 1024;

    explicit TimelineComponent(Allocator& allocator) noexcept;

    // --- 재생(UE FTimeline Play·Reverse·Stop — 32.3절) ---

    // 앞으로 재생한다(Tick 켬).
    void Play() noexcept;
    // 위치 0으로(이벤트 없이) 옮긴 뒤 앞으로 재생한다.
    void PlayFromStart() noexcept;
    // 뒤로 재생한다(Tick 켬).
    void Reverse() noexcept;
    // 끝(GetTimelineLength)으로 옮긴 뒤 뒤로 재생한다.
    void ReverseFromEnd() noexcept;
    // 멈춘다(finished는 오지 않는다). Tick은 다음 Tick에서 끈다.
    void Stop() noexcept;

    bool IsPlaying() const noexcept {
        return playing_;
    }

    bool IsReversing() const noexcept {
        return playing_ && reversePlayback_;
    }

    // 위치를 바꾸고 트랙을 평가해 알린다(UE SetPlaybackPosition — 32.3절). fireEvents면 옛 위치와
    // 새 위치 사이의 이벤트 키를, fireUpdate면 updated를 발행한다.
    void SetPlaybackPosition(float newPosition, bool fireEvents, bool fireUpdate = true) noexcept;

    float GetPlaybackPosition() const noexcept {
        return position_;
    }

    // [0, length]로 자른 time으로 옮긴다(이벤트 키 없음 — UE SetNewTime).
    void SetNewTime(float time) noexcept;

    // delta초만큼 재생한다(UE TickTimeline — 컴포넌트 Tick이 부른다, 시험·도구도 부를 수 있다).
    void TickTimeline(float deltaSeconds) noexcept;

    // --- 설정 ---

    bool IsLooping() const noexcept {
        return looping_;
    }

    void SetLooping(bool looping) noexcept {
        looping_ = looping;
    }

    float GetPlayRate() const noexcept {
        return playRate_;
    }

    void SetPlayRate(float rate) noexcept {
        playRate_ = rate;
    }

    TimelineLengthMode GetTimelineLengthMode() const noexcept {
        return lengthMode_;
    }

    void SetTimelineLengthMode(TimelineLengthMode mode) noexcept {
        lengthMode_ = mode;
    }

    // 지금 길이다(TimelineLength면 length, LastKeyFrame면 이벤트 키·트랙 커브의 마지막 키 시간 —
    // 커브가 없거나 비었으면 0).
    float GetTimelineLength() const noexcept;

    // 길이 / playRate다(playRate 0이면 0 — UE는 Error 로그).
    float GetScaledTimelineLength() const noexcept;

    float GetLengthSetting() const noexcept {
        return length_;
    }

    // length를 바꾼다. 위치가 새 길이보다 뒤면 SetNewTime(새 길이 − EventEdgeEpsilon)이다(UE).
    void SetTimelineLength(float length) noexcept;

    bool GetIgnoreTimeDilation() const noexcept {
        return ignoreTimeDilation_;
    }

    // 참이면 컴포넌트 Tick이 unscaled delta(시간 배율·pause 배율 무시)를 쓴다(UE
    // bIgnoreTimeDilation).
    void SetIgnoreTimeDilation(bool ignore) noexcept {
        ignoreTimeDilation_ = ignore;
    }

    // --- 트랙(UE AddInterpFloat 등 — 인라인 커브 사본) ---

    // 트랙을 끝에 더한다(커브를 복사한다). 오류: 할당 실패(그대로).
    Result<void> TryAddFloatTrack(Name trackName, const TangentCurve& curve) noexcept;
    Result<void> TryAddVectorTrack(Name trackName, const VectorCurveValue& curve) noexcept;
    Result<void> TryAddColorTrack(Name trackName, const ColorCurveValue& curve) noexcept;
    // 이벤트 키를 끝에 더한다. 오류: time 비유한 InvalidArgument, 할당 실패.
    Result<void> TryAddEventKey(float time, Name eventName) noexcept;

    Span<const TimelineFloatTrack> FloatTracks() const noexcept {
        return floatTracks_.View();
    }

    Span<const TimelineVectorTrack> VectorTracks() const noexcept {
        return vectorTracks_.View();
    }

    Span<const TimelineColorTrack> ColorTracks() const noexcept {
        return colorTracks_.View();
    }

    Span<const TimelineEventKey> EventKeys() const noexcept {
        return eventKeys_.View();
    }

    // 이름이 같은 첫 트랙의 curveAsset을 바꾸고 그 트랙의 로드 사본을 지운다(UE
    // SetFloatCurve(curve, trackName)). 같은 값이면 아무것도 하지 않는다(사본 유지). null이면
    // 인라인 커브로 돌아간다(UE는 null을 거부한다 — 32.6절). 오류: 이름이 None이거나 그런 트랙이
    // 없음 NotFound(UE는 Log), 잘못된 soft 참조 값·할당 실패(그대로).
    Result<void> SetFloatTrackCurveAsset(Name trackName,
                                         const SoftObjectRef<FloatCurveAsset>& asset) noexcept;
    Result<void> SetVectorTrackCurveAsset(Name trackName,
                                          const SoftObjectRef<VectorCurveAsset>& asset) noexcept;
    Result<void> SetColorTrackCurveAsset(Name trackName,
                                         const SoftObjectRef<ColorCurveAsset>& asset) noexcept;

    // --- 로드 사본(ERD71 — 30.3절 ApplyLoadedMeshInfo와 같은 규칙) ---

    // 로드 시스템이 lease를 얻은 뒤 요청에 쓴 참조(requested)와 함께 부른다. 트랙 커브를 값으로
    // 복사한다(에셋 객체를 쥐지 않는다). 색은 에셋의 색 조정을 적용하는 값이다. 오류: trackIndex
    // 범위 밖, requested가 null이거나 그 트랙의 curveAsset과 다름(InvalidArgument — 늦게 끝난 옛
    // 요청, 값 유지), 할당 실패(그대로).
    Result<void> ApplyLoadedFloatCurve(usize trackIndex,
                                       const SoftObjectRef<FloatCurveAsset>& requested,
                                       const FloatCurveAsset& asset) noexcept;
    Result<void> ApplyLoadedVectorCurve(usize trackIndex,
                                        const SoftObjectRef<VectorCurveAsset>& requested,
                                        const VectorCurveAsset& asset) noexcept;
    Result<void> ApplyLoadedColorCurve(usize trackIndex,
                                       const SoftObjectRef<ColorCurveAsset>& requested,
                                       const ColorCurveAsset& asset) noexcept;

    // 로드 사본을 지운다(에셋을 놓을 때 로드 시스템이 부른다). 범위 밖 trackIndex는 아무것도 하지
    // 않는다.
    void ClearLoadedFloatCurve(usize trackIndex) noexcept;
    void ClearLoadedVectorCurve(usize trackIndex) noexcept;
    void ClearLoadedColorCurve(usize trackIndex) noexcept;

    // 트랙에 평가할 커브가 있는지다(인라인 트랙은 늘 참, 에셋 트랙은 로드 사본이 있을 때).
    bool HasFloatCurve(usize trackIndex) const noexcept;
    bool HasVectorCurve(usize trackIndex) const noexcept;
    bool HasColorCurve(usize trackIndex) const noexcept;

    // --- 이벤트(ERD72 — 32.2절) ---

    // float 트랙 값이다(트랙 순서대로, 위치가 바뀔 때마다).
    ROCKS_PROPERTY(Params = "trackName,value") Event<Name, float> floatTrackUpdated;
    ROCKS_PROPERTY(Params = "trackName,value") Event<Name, Vec3d> vectorTrackUpdated;
    ROCKS_PROPERTY(Params = "trackName,value") Event<Name, LinearColor> colorTrackUpdated;
    // 재생이 이벤트 키 시간을 지났다(UE Events[i].EventFunc).
    ROCKS_PROPERTY(Params = "eventName") Event<Name> eventKeyReached;
    // 트랙 평가·키 뒤에 한 번이다(UE TimelinePostUpdateFunc).
    ROCKS_PROPERTY() Event<> updated;
    // 끝까지 재생해 멈췄다(Stop으로 멈추면 오지 않는다 — UE TimelineFinishedFunc).
    ROCKS_PROPERTY() Event<> finished;

protected:
    void OnStart() noexcept override;
    void OnTick(const TickContext& context) noexcept override;

private:
    // 재생 상태를 바꾸고 Tick을 켠다(UE 컴포넌트 Activate + ChangeMarkPlayingState).
    void StartPlaying(bool reverse) noexcept;
    // 트랙을 평가해 이벤트를 발행한다(SetPlaybackPosition 2단계).
    void BroadcastTracks() noexcept;
    // 이벤트 문맥 없이(World 밖) 발행하지 않는다.
    bool CanBroadcast() const noexcept;
    float LastKeyframeTime() const noexcept;

    // 설정·상태(UE FTimeline UPROPERTY — 저장한다).
    ROCKS_PROPERTY(ReadOnly, Name = "lengthMode")
    TimelineLengthMode lengthMode_ = TimelineLengthMode::LastKeyFrame;
    ROCKS_PROPERTY(ReadOnly, Name = "length") float length_ = DefaultLength;
    ROCKS_PROPERTY(ReadOnly, Name = "looping") bool looping_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "reversePlayback") bool reversePlayback_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "playing") bool playing_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "playRate") float playRate_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "position") float position_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "ignoreTimeDilation") bool ignoreTimeDilation_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "floatTracks") Array<TimelineFloatTrack> floatTracks_;
    ROCKS_PROPERTY(ReadOnly, Name = "vectorTracks") Array<TimelineVectorTrack> vectorTracks_;
    ROCKS_PROPERTY(ReadOnly, Name = "colorTracks") Array<TimelineColorTrack> colorTracks_;
    ROCKS_PROPERTY(ReadOnly, Name = "eventKeys") Array<TimelineEventKey> eventKeys_;

    // 로드 사본(저장하지 않는다 — 트랙 번호와 같은 자리, 짧으면 그 뒤 트랙은 사본 없음).
    ROCKS_NATIVE() Array<TangentCurve> loadedFloatCurves_;
    ROCKS_NATIVE() Array<VectorCurveValue> loadedVectorCurves_;
    ROCKS_NATIVE() Array<ColorCurveValue> loadedColorCurves_;
    ROCKS_NATIVE() Array<uint8> loadedFloat_;
    ROCKS_NATIVE() Array<uint8> loadedVector_;
    ROCKS_NATIVE() Array<uint8> loadedColor_;
};

} // namespace rocks
