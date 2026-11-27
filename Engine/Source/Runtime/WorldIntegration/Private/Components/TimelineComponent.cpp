#include <Core/Math/MathUtility.h>
#include <CurveAssets/ColorCurveEvaluate.h>
#include <World/Component.generated.h>
#include <World/World.h>
#include <WorldIntegration/Components/TimelineComponent.generated.h>
#include <WorldIntegration/Components/TimelineComponent.h>

// 타임라인 컴포넌트다(Docs/Design/EngineRuntime 32장 — UE FTimeline·UTimelineComponent,
// Engine/Private/Timeline.cpp). 연산 순서·float 계산은 UE 그대로다.
namespace rocks {

namespace {

// UE FMath::Clamp(X < Min ? Min : X < Max ? X : Max — NaN이면 Max).
float ClampTime(float value, float low, float high) noexcept {
    return value < low ? low : (value < high ? value : high);
}

// UE FMath::Max(A >= B ? A : B).
float MaxTime(float left, float right) noexcept {
    return left >= right ? left : right;
}

// curve의 마지막 키 시간이다(키가 없으면 0 — UE FRichCurve::GetTimeRange).
float LastKeyTime(const TangentCurve& curve) noexcept {
    auto range = curve.TryGetTimeRange();
    return range ? range.Value().max : 0.0f;
}

// 로드 사본 자리를 index까지 늘린다(할당을 먼저 잡고 실패하면 그대로다).
template <class Value>
Result<void> GrowLoaded(Array<Value>& values, Array<uint8>& flags, usize index,
                        Allocator& allocator) noexcept {
    if (index < values.Size()) {
        return Result<void>::Success();
    }

    if (auto reserved = values.TryReserve(index + 1); !reserved) {
        return reserved;
    }

    if (auto reserved = flags.TryReserve(index + 1); !reserved) {
        return reserved;
    }

    while (values.Size() <= index) {
        (void)values.TryPushBack(Value(allocator));
        (void)flags.TryPushBack(uint8{0});
    }

    return Result<void>::Success();
}

bool IsLoaded(const Array<uint8>& flags, usize index) noexcept {
    return index < flags.Size() && flags[index] != 0;
}

} // namespace

TimelineComponent::TimelineComponent(Allocator& allocator) noexcept
    : Component(allocator), floatTrackUpdated(allocator), vectorTrackUpdated(allocator),
      colorTrackUpdated(allocator), eventKeyReached(allocator), updated(allocator),
      finished(allocator), floatTracks_(allocator), vectorTracks_(allocator),
      colorTracks_(allocator), eventKeys_(allocator), loadedFloatCurves_(allocator),
      loadedVectorCurves_(allocator), loadedColorCurves_(allocator), loadedFloat_(allocator),
      loadedVector_(allocator), loadedColor_(allocator) {
    // UE UTimelineComponent 생성자(Timeline.cpp:778–784): Tick은 하지만 처음에는 꺼져 있다,
    // PrePhysics.
    PrimaryTick().tick = true;
    PrimaryTick().phase = TickPhase::PrePhysics;
    (void)SetTickEnabled(false);
}

// --- 재생 ---

void TimelineComponent::StartPlaying(bool reverse) noexcept {
    reversePlayback_ = reverse;
    playing_ = true;
    (void)SetTickEnabled(true);
}

void TimelineComponent::Play() noexcept {
    StartPlaying(false);
}

void TimelineComponent::PlayFromStart() noexcept {
    SetPlaybackPosition(0.0f, false);
    Play();
}

void TimelineComponent::Reverse() noexcept {
    StartPlaying(true);
}

void TimelineComponent::ReverseFromEnd() noexcept {
    SetPlaybackPosition(GetTimelineLength(), false);
    Reverse();
}

void TimelineComponent::Stop() noexcept {
    playing_ = false;
}

bool TimelineComponent::CanBroadcast() const noexcept {
    return HasWorld();
}

void TimelineComponent::BroadcastTracks() noexcept {
    if (!CanBroadcast()) {
        return;
    }

    World& world = GetWorld();
    const EventContext context{world.Objects(), world.Access(), Handle()};
    // UE 순서: 벡터 → float → 색(Timeline.cpp:281–381). 구독자가 트랙을 더해도 번호로 다시 읽는다.
    if (vectorTrackUpdated.IsBound()) {
        for (usize index = 0; index < vectorTracks_.Size(); ++index) {
            if (!HasVectorCurve(index)) {
                continue;
            }

            const TimelineVectorTrack& track = vectorTracks_[index];
            const VectorCurveValue& curve =
                track.curveAsset.IsNull() ? track.curve : loadedVectorCurves_[index];
            const Name name = track.trackName;
            const Vec3d value = curve.Evaluate(position_);
            vectorTrackUpdated.Broadcast(context, name, value);
        }
    }

    if (floatTrackUpdated.IsBound()) {
        for (usize index = 0; index < floatTracks_.Size(); ++index) {
            if (!HasFloatCurve(index)) {
                continue;
            }

            const TimelineFloatTrack& track = floatTracks_[index];
            const TangentCurve& curve =
                track.curveAsset.IsNull() ? track.curve : loadedFloatCurves_[index];
            const Name name = track.trackName;
            const float value = curve.Evaluate(position_);
            floatTrackUpdated.Broadcast(context, name, value);
        }
    }

    if (colorTrackUpdated.IsBound()) {
        for (usize index = 0; index < colorTracks_.Size(); ++index) {
            if (!HasColorCurve(index)) {
                continue;
            }

            const TimelineColorTrack& track = colorTracks_[index];
            const ColorCurveValue& curve =
                track.curveAsset.IsNull() ? track.curve : loadedColorCurves_[index];
            const Name name = track.trackName;
            // UE GetLinearColorValue(늘 조정 적용 식 — 기본 조정이어도 HSV 왕복과 LDR 명도
            // 자르기)다.
            const LinearColor value = EvaluateAdjustedColorCurve(
                curve.r, curve.g, curve.b, curve.a, curve.Adjustments(), position_, false);
            colorTrackUpdated.Broadcast(context, name, value);
        }
    }
}

// UE FTimeline::SetPlaybackPosition(Timeline.cpp:274–500).
void TimelineComponent::SetPlaybackPosition(float newPosition, bool fireEvents,
                                            bool fireUpdate) noexcept {
    const float oldPosition = position_;
    position_ = newPosition;
    BroadcastTracks();
    if (!CanBroadcast()) {
        return;
    }

    World& world = GetWorld();
    const EventContext context{world.Objects(), world.Access(), Handle()};
    if (fireEvents) {
        float minimum;
        float maximum;
        if (!reversePlayback_) {
            minimum = oldPosition;
            maximum = position_;
            // 끝에 닿으면 끝 키도 발행되게 조금 넓힌다(UE).
            if (maximum == GetTimelineLength()) {
                maximum += EventEdgeEpsilon;
            }
        } else {
            minimum = position_;
            maximum = oldPosition;
            if (minimum == 0.0f) {
                minimum -= EventEdgeEpsilon;
            }
        }

        for (usize index = 0; index < eventKeys_.Size(); ++index) {
            const float time = eventKeys_[index].time;
            const bool fire = !reversePlayback_ ? (time >= minimum && time < maximum)
                                                : (time > minimum && time <= maximum);
            if (fire) {
                const Name name = eventKeys_[index].eventName;
                eventKeyReached.Broadcast(context, name);
            }
        }
    }

    if (fireUpdate) {
        updated.Broadcast(context);
    }
}

void TimelineComponent::SetNewTime(float time) noexcept {
    SetPlaybackPosition(ClampTime(time, 0.0f, length_), false);
}

// UE FTimeline::TickTimeline(Timeline.cpp:516–599).
void TimelineComponent::TickTimeline(float deltaSeconds) noexcept {
    bool reachedEnd = false;
    if (playing_) {
        const float timelineLength = GetTimelineLength();
        const float effectiveDelta = deltaSeconds * (reversePlayback_ ? (-playRate_) : playRate_);
        // 비유한 delta·배율은 이번 Tick을 건너뛴다(32.6절 3 — UE는 위치가 NaN이 된다).
        if (!math::IsFinite(effectiveDelta)) {
            return;
        }

        float newPosition = position_ + effectiveDelta;
        // 위치가 비유한이면(SetPlaybackPosition(Inf) 뒤·합의 넘침) 이번 Tick을 건너뛴다.
        if (!math::IsFinite(newPosition)) {
            return;
        }

        if (effectiveDelta > 0.0f) {
            if (newPosition > timelineLength) {
                if (looping_) {
                    SetPlaybackPosition(timelineLength, true, false);
                    SetPlaybackPosition(0.0f, false, false);
                    if (timelineLength > 0.0f) {
                        uint32 wraps = 0;
                        while (newPosition > timelineLength) {
                            if (++wraps > MaxTimelineLoopWraps) {
                                newPosition = math::Fmod(newPosition, timelineLength);
                                break;
                            }

                            newPosition -= timelineLength;
                        }
                    } else {
                        newPosition = 0.0f;
                    }
                } else {
                    newPosition = timelineLength;
                    Stop();
                    reachedEnd = true;
                }
            }
        } else {
            if (newPosition < 0.0f) {
                if (looping_) {
                    SetPlaybackPosition(0.0f, true, false);
                    SetPlaybackPosition(timelineLength, false, false);
                    if (timelineLength > 0.0f) {
                        uint32 wraps = 0;
                        while (newPosition < 0.0f) {
                            if (++wraps > MaxTimelineLoopWraps) {
                                newPosition = math::Fmod(newPosition, timelineLength);
                                if (newPosition < 0.0f) {
                                    newPosition += timelineLength;
                                }

                                break;
                            }

                            newPosition += timelineLength;
                        }
                    } else {
                        newPosition = 0.0f;
                    }
                } else {
                    newPosition = 0.0f;
                    Stop();
                    reachedEnd = true;
                }
            }
        }

        SetPlaybackPosition(newPosition, true);
    }

    if (reachedEnd && CanBroadcast()) {
        World& world = GetWorld();
        finished.Broadcast({world.Objects(), world.Access(), Handle()});
    }
}

// 저장된 재생 상태(playing — 로드·복제)로 시작하면 Tick을 켠다(32.1절 — Tick 켜짐은 저장하지
// 않는다).
void TimelineComponent::OnStart() noexcept {
    if (playing_) {
        (void)SetTickEnabled(true);
    }
}

// UE UTimelineComponent::TickComponent(Timeline.cpp:786–814).
void TimelineComponent::OnTick(const TickContext& context) noexcept {
    const double delta = ignoreTimeDilation_ ? context.unscaledDeltaSeconds : context.deltaSeconds;
    TickTimeline(static_cast<float>(delta));
    if (!playing_) {
        (void)SetTickEnabled(false);
    }
}

// --- 길이 ---

float TimelineComponent::LastKeyframeTime() const noexcept {
    // UE FTimeline::GetLastKeyframeTime(Timeline.cpp:676–709) — 커브 없는 트랙은 0.
    float maximum = 0.0f;
    for (const TimelineEventKey& key : eventKeys_) {
        maximum = MaxTime(key.time, maximum);
    }

    for (usize index = 0; index < vectorTracks_.Size(); ++index) {
        if (!HasVectorCurve(index)) {
            continue;
        }

        const TimelineVectorTrack& track = vectorTracks_[index];
        const VectorCurveValue& curve =
            track.curveAsset.IsNull() ? track.curve : loadedVectorCurves_[index];
        maximum = MaxTime(LastKeyTime(curve.x), maximum);
        maximum = MaxTime(LastKeyTime(curve.y), maximum);
        maximum = MaxTime(LastKeyTime(curve.z), maximum);
    }

    for (usize index = 0; index < floatTracks_.Size(); ++index) {
        if (!HasFloatCurve(index)) {
            continue;
        }

        const TimelineFloatTrack& track = floatTracks_[index];
        const TangentCurve& curve =
            track.curveAsset.IsNull() ? track.curve : loadedFloatCurves_[index];
        maximum = MaxTime(LastKeyTime(curve), maximum);
    }

    for (usize index = 0; index < colorTracks_.Size(); ++index) {
        if (!HasColorCurve(index)) {
            continue;
        }

        const TimelineColorTrack& track = colorTracks_[index];
        const ColorCurveValue& curve =
            track.curveAsset.IsNull() ? track.curve : loadedColorCurves_[index];
        maximum = MaxTime(LastKeyTime(curve.r), maximum);
        maximum = MaxTime(LastKeyTime(curve.g), maximum);
        maximum = MaxTime(LastKeyTime(curve.b), maximum);
        maximum = MaxTime(LastKeyTime(curve.a), maximum);
    }

    return maximum;
}

float TimelineComponent::GetTimelineLength() const noexcept {
    return lengthMode_ == TimelineLengthMode::TimelineLength ? length_ : LastKeyframeTime();
}

float TimelineComponent::GetScaledTimelineLength() const noexcept {
    return playRate_ != 0.0f ? GetTimelineLength() / playRate_ : 0.0f;
}

void TimelineComponent::SetTimelineLength(float length) noexcept {
    length_ = length;
    if (position_ > length) {
        SetNewTime(length - EventEdgeEpsilon);
    }
}

// --- 트랙 ---

Result<void> TimelineComponent::TryAddFloatTrack(Name trackName,
                                                 const TangentCurve& curve) noexcept {
    TimelineFloatTrack track(floatTracks_.GetAllocator());
    track.trackName = trackName;
    if (auto copied = track.curve.TryCopyFrom(curve); !copied) {
        return copied;
    }

    return floatTracks_.TryPushBack(Move(track));
}

Result<void> TimelineComponent::TryAddVectorTrack(Name trackName,
                                                  const VectorCurveValue& curve) noexcept {
    TimelineVectorTrack track(vectorTracks_.GetAllocator());
    track.trackName = trackName;
    if (auto copied = track.curve.TryCopyFrom(curve); !copied) {
        return copied;
    }

    return vectorTracks_.TryPushBack(Move(track));
}

Result<void> TimelineComponent::TryAddColorTrack(Name trackName,
                                                 const ColorCurveValue& curve) noexcept {
    TimelineColorTrack track(colorTracks_.GetAllocator());
    track.trackName = trackName;
    if (auto copied = track.curve.TryCopyFrom(curve); !copied) {
        return copied;
    }

    return colorTracks_.TryPushBack(Move(track));
}

Result<void> TimelineComponent::TryAddEventKey(float time, Name eventName) noexcept {
    if (!math::IsFinite(time)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TimelineEventKey key;
    key.time = time;
    key.eventName = eventName;
    return eventKeys_.TryPushBack(key);
}

namespace {

// 이름이 같은 첫 트랙의 번호다(None·없으면 IndexNone).
template <class Track>
usize FindTrack(const Array<Track>& tracks, Name name) noexcept {
    if (name.IsNone()) {
        return IndexNone;
    }

    for (usize index = 0; index < tracks.Size(); ++index) {
        if (tracks[index].trackName == name) {
            return index;
        }
    }

    return IndexNone;
}

template <class Track, class Asset>
Result<void> SetTrackAsset(Array<Track>& tracks, Array<uint8>& loaded, Name name,
                           const SoftObjectRef<Asset>& asset) noexcept {
    const usize index = FindTrack(tracks, name);
    if (index == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 같은 값이면 로드 사본을 지우지 않는다(30.3절 SetStaticMesh와 같다 — 로드 시스템은 바뀌었을
    // 때만 다시 요청한다).
    if (tracks[index].curveAsset == asset) {
        return Result<void>::Success();
    }

    if (auto assigned = tracks[index].curveAsset.TryAssign(asset); !assigned) {
        return assigned;
    }

    if (index < loaded.Size()) {
        loaded[index] = 0;
    }

    return Result<void>::Success();
}

// requested가 그 트랙의 지금 curveAsset인지다(늦게 끝난 옛 요청을 거른다).
template <class Track, class Asset>
bool MatchesRequest(const Array<Track>& tracks, usize index,
                    const SoftObjectRef<Asset>& requested) noexcept {
    return index < tracks.Size() && !requested.IsNull() && tracks[index].curveAsset == requested;
}

} // namespace

Result<void>
TimelineComponent::SetFloatTrackCurveAsset(Name trackName,
                                           const SoftObjectRef<FloatCurveAsset>& asset) noexcept {
    return SetTrackAsset(floatTracks_, loadedFloat_, trackName, asset);
}

Result<void>
TimelineComponent::SetVectorTrackCurveAsset(Name trackName,
                                            const SoftObjectRef<VectorCurveAsset>& asset) noexcept {
    return SetTrackAsset(vectorTracks_, loadedVector_, trackName, asset);
}

Result<void>
TimelineComponent::SetColorTrackCurveAsset(Name trackName,
                                           const SoftObjectRef<ColorCurveAsset>& asset) noexcept {
    return SetTrackAsset(colorTracks_, loadedColor_, trackName, asset);
}

// --- 로드 사본 ---

Result<void>
TimelineComponent::ApplyLoadedFloatCurve(usize trackIndex,
                                         const SoftObjectRef<FloatCurveAsset>& requested,
                                         const FloatCurveAsset& asset) noexcept {
    if (!MatchesRequest(floatTracks_, trackIndex, requested)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = floatTracks_.GetAllocator();
    TangentCurve copy(allocator);
    if (auto copied = copy.TryCopyFrom(asset.curve); !copied) {
        return copied;
    }

    if (auto grown = GrowLoaded(loadedFloatCurves_, loadedFloat_, trackIndex, allocator); !grown) {
        return grown;
    }

    loadedFloatCurves_[trackIndex] = Move(copy);
    loadedFloat_[trackIndex] = 1;
    return Result<void>::Success();
}

Result<void>
TimelineComponent::ApplyLoadedVectorCurve(usize trackIndex,
                                          const SoftObjectRef<VectorCurveAsset>& requested,
                                          const VectorCurveAsset& asset) noexcept {
    if (!MatchesRequest(vectorTracks_, trackIndex, requested)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = vectorTracks_.GetAllocator();
    VectorCurveValue copy(allocator);
    if (auto copied = copy.x.TryCopyFrom(asset.x); !copied) {
        return copied;
    }

    if (auto copied = copy.y.TryCopyFrom(asset.y); !copied) {
        return copied;
    }

    if (auto copied = copy.z.TryCopyFrom(asset.z); !copied) {
        return copied;
    }

    if (auto grown = GrowLoaded(loadedVectorCurves_, loadedVector_, trackIndex, allocator);
        !grown) {
        return grown;
    }

    loadedVectorCurves_[trackIndex] = Move(copy);
    loadedVector_[trackIndex] = 1;
    return Result<void>::Success();
}

Result<void>
TimelineComponent::ApplyLoadedColorCurve(usize trackIndex,
                                         const SoftObjectRef<ColorCurveAsset>& requested,
                                         const ColorCurveAsset& asset) noexcept {
    if (!MatchesRequest(colorTracks_, trackIndex, requested)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = colorTracks_.GetAllocator();
    ColorCurveValue copy(allocator);
    if (auto copied = copy.r.TryCopyFrom(asset.r); !copied) {
        return copied;
    }

    if (auto copied = copy.g.TryCopyFrom(asset.g); !copied) {
        return copied;
    }

    if (auto copied = copy.b.TryCopyFrom(asset.b); !copied) {
        return copied;
    }

    if (auto copied = copy.a.TryCopyFrom(asset.a); !copied) {
        return copied;
    }

    // 에셋 색은 조정 적용 값이다(UE GetLinearColorValue).
    copy.SetAdjustments(asset.Adjustments(), true);
    if (auto grown = GrowLoaded(loadedColorCurves_, loadedColor_, trackIndex, allocator); !grown) {
        return grown;
    }

    loadedColorCurves_[trackIndex] = Move(copy);
    loadedColor_[trackIndex] = 1;
    return Result<void>::Success();
}

void TimelineComponent::ClearLoadedFloatCurve(usize trackIndex) noexcept {
    if (trackIndex < loadedFloat_.Size()) {
        loadedFloat_[trackIndex] = 0;
    }
}

void TimelineComponent::ClearLoadedVectorCurve(usize trackIndex) noexcept {
    if (trackIndex < loadedVector_.Size()) {
        loadedVector_[trackIndex] = 0;
    }
}

void TimelineComponent::ClearLoadedColorCurve(usize trackIndex) noexcept {
    if (trackIndex < loadedColor_.Size()) {
        loadedColor_[trackIndex] = 0;
    }
}

bool TimelineComponent::HasFloatCurve(usize trackIndex) const noexcept {
    return trackIndex < floatTracks_.Size() &&
           (floatTracks_[trackIndex].curveAsset.IsNull() || IsLoaded(loadedFloat_, trackIndex));
}

bool TimelineComponent::HasVectorCurve(usize trackIndex) const noexcept {
    return trackIndex < vectorTracks_.Size() &&
           (vectorTracks_[trackIndex].curveAsset.IsNull() || IsLoaded(loadedVector_, trackIndex));
}

bool TimelineComponent::HasColorCurve(usize trackIndex) const noexcept {
    return trackIndex < colorTracks_.Size() &&
           (colorTracks_[trackIndex].curveAsset.IsNull() || IsLoaded(loadedColor_, trackIndex));
}

} // namespace rocks
