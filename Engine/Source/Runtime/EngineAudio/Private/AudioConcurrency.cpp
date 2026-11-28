#include "AudioDeviceContext.h"

#include <Audio/Decibels.h>
#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

// 동시 발음 그룹이다(Docs/Design/AudioSystem 3.41.2절, UE FSoundConcurrencyManager 대응). 멤버십은
// 재생 기록(links)이 쥐고 그룹 표는 수·설정·마지막 재생 시각만 둔다. 그룹의 "목록"은 멤버를 수락
// 순번으로 정렬한 것이다(UE ActiveSounds 배열 순서 대신 — 결정적). 그래서 수락·cull 평가는 할당
// 없이 재생 기록을 훑고, UE 루프를 그 목록 위에서 그대로 돈다.
namespace rocks {

namespace {

bool IsCullRule(SoundConcurrencyRule rule) noexcept {
    return rule == SoundConcurrencyRule::StopLowestPriority ||
           rule == SoundConcurrencyRule::StopQuietest ||
           rule == SoundConcurrencyRule::StopLowestPriorityThenPreventNew;
}

// 선형값의 dB다(0 이하·floor 아래는 floorDb).
float ToDecibels(float linear, float floorDb) noexcept {
    return linear > 0.0f
               ? math::Max(audio::LinearToDecibels(linear, audio::DecibelsToLinear(floorDb)),
                           floorDb)
               : floorDb;
}

// volume scale 목표 V^exponent다(UE FMath::Pow).
float ScalePower(float base, uint32 exponent) noexcept {
    return exponent == 0 ? 1.0f : platform::PlatformMath::Pow(base, static_cast<float>(exponent));
}

bool NearlyEqual(double left, double right, double tolerance) noexcept {
    return math::Abs(left - right) <= tolerance;
}

} // namespace

void AudioDeviceContext::DecibelRamp::Set(float linear, float floor) noexcept {
    floorDb = floor;
    fromDb = ToDecibels(linear, floorDb);
    toDb = fromDb;
    frames = 0;
}

void AudioDeviceContext::DecibelRamp::StartTo(float targetLinear, uint64 frame,
                                              uint32 length) noexcept {
    fromDb = ToDecibels(At(frame), floorDb);
    toDb = ToDecibels(targetLinear, floorDb);
    startFrame = frame;
    frames = length;
}

float AudioDeviceContext::DecibelRamp::At(uint64 frame) const noexcept {
    float db = toDb;
    if (frames != 0 && frame < startFrame + frames) {
        const float t = frame <= startFrame
                            ? 0.0f
                            : static_cast<float>(frame - startFrame) / static_cast<float>(frames);
        db = fromDb + (toDb - fromDb) * t;
    }

    return audio::DecibelsToLinear(db);
}

bool AudioDeviceContext::DecibelRamp::IsRamping(uint64 frame) const noexcept {
    return frames != 0 && frame < startFrame + frames;
}

uint32 AudioDeviceContext::SecondsToMixFrames(double seconds) const noexcept {
    const double frames = seconds * static_cast<double>(mixer_.Config().sampleRate);
    const double limit = static_cast<double>(NumericLimits<uint32>::Max());
    return frames <= 0.0 ? 0u : static_cast<uint32>(math::Min(frames + 0.5, limit));
}

double AudioDeviceContext::ListenerDistanceSquared(const Record& record) const noexcept {
    // 2D 재생은 위치가 없어 0이다(UE는 ActiveSound Transform — 3.41.2절, 7절). emitter 재생은
    // 감쇠가 없어도 emitter의 지금 위치다(공간 갱신은 감쇠 있는 재생만 한다).
    if (record.placement == PlayPlacement::NonSpatial) {
        return 0.0;
    }

    Vec3d position = record.position;
    if (record.placement == PlayPlacement::Emitter && record.emitter.slot < emitters_.Size() &&
        emitters_[record.emitter.slot].generation == record.emitter.generation) {
        position = emitters_[record.emitter.slot].position;
    }

    return (position - scenes_[record.scene].listenerPosition).LengthSquared();
}

uint32 AudioDeviceContext::FindConcurrencyGroup(ConcurrencyMode mode, uint64 owner,
                                                uint64 key) const noexcept {
    for (uint32 index = 0; index < groups_.Size(); ++index) {
        const ConcurrencyGroupState& group = groups_[index];
        if (group.used && group.mode == mode && group.owner == owner && group.key == key) {
            return index;
        }
    }

    return NumericLimits<uint32>::Max();
}

void AudioDeviceContext::GatherGroupMembers(uint32 group, uint32 except) noexcept {
    groupScratch_.Clear();
    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        const Record& record = records_[slot];
        if (slot == except || record.state == RecordState::Inactive) {
            continue;
        }

        for (uint32 link = 0; link < record.linkCount; ++link) {
            if (record.links[link].group == group) {
                (void)groupScratch_.TryPushBack(slot);
                break;
            }
        }
    }

    // 그룹 목록 순서 = 수락 순번(3.41.2절).
    const Array<Record>& records = records_;
    algo::Sort(Span<uint32>(groupScratch_.Data(), groupScratch_.Size()),
               [&](uint32 left, uint32 right) noexcept {
                   return records[left].sequence < records[right].sequence;
               });
}

AudioDeviceContext::ConcurrencyOutcome
AudioDeviceContext::EvaluateConcurrency(uint32 slot, ControlBatch& batch, uint64 now,
                                        bool realization) noexcept {
    const Record& command = records_[slot];
    // UE EvaluateConcurrency(SC 818–899): 그룹마다 CanPlay(AND — 첫 거부에서 멈춘다), 희생자는 모은
    // 뒤 확정한다. UE CreateAndEvictActiveSounds(SC 902–990)처럼 새 재생을 그룹에 먼저
    // 넣고(ducking) 그 다음 희생자를 뺀다.
    uint32 groupOf[MaxConcurrencyGroups];
    ConcurrencyMode modeOf[MaxConcurrencyGroups];
    uint64 ownerOf[MaxConcurrencyGroups];
    uint32 victims[MaxConcurrencyGroups];
    uint32 victimCount = 0;
    uint32 missing = 0;
    for (uint32 index = 0; index < command.concurrencyCount; ++index) {
        const SoundConcurrencyEntry& entry = command.concurrency[index];
        const bool perOwner = entry.settings.limitToOwner && command.owner != 0;
        modeOf[index] = entry.isOverride
                            ? (perOwner ? ConcurrencyMode::OwnerPerSound : ConcurrencyMode::Sound)
                            : (perOwner ? ConcurrencyMode::Owner : ConcurrencyMode::Group);
        ownerOf[index] = perOwner ? command.owner : 0;
        groupOf[index] = FindConcurrencyGroup(modeOf[index], ownerOf[index], entry.key);
        if (groupOf[index] == NumericLimits<uint32>::Max()) {
            // 없는 그룹은 평가 뒤에 만든다(새 그룹에서는 첫 재생이 늘 통과 — SC 897).
            ++missing;
            continue;
        }

        uint32 victim = NumericLimits<uint32>::Max();
        if (!CanPlayInGroup(groupOf[index], slot, realization, now, victim)) {
            return ConcurrencyOutcome::Rejected;
        }

        if (victim != NumericLimits<uint32>::Max()) {
            bool seen = false;
            for (uint32 at = 0; at < victimCount; ++at) {
                seen = seen || victims[at] == victim;
            }

            if (!seen) {
                victims[victimCount++] = victim;
            }
        }
    }

    // 새 그룹 칸이 모자라면 아무도 빼앗기 전에 실패한다(표 용량 — Failed).
    uint32 free = 0;
    for (const ConcurrencyGroupState& group : groups_) {
        free += group.used ? 0u : 1u;
    }

    if (free < missing) {
        return ConcurrencyOutcome::TableFull;
    }

    for (uint32 index = 0; index < command.concurrencyCount; ++index) {
        uint32 group = groupOf[index];
        if (group == NumericLimits<uint32>::Max()) {
            group =
                FindConcurrencyGroup(modeOf[index], ownerOf[index], command.concurrency[index].key);
        }

        if (group == NumericLimits<uint32>::Max()) {
            for (uint32 candidate = 0; candidate < groups_.Size(); ++candidate) {
                if (!groups_[candidate].used) {
                    group = candidate;
                    break;
                }
            }

            ROCKS_ASSERT(group != NumericLimits<uint32>::Max());
            ConcurrencyGroupState& made = groups_[group];
            made = ConcurrencyGroupState{};
            made.used = true;
            made.mode = modeOf[index];
            made.owner = ownerOf[index];
            made.key = command.concurrency[index].key;
            made.settings = command.concurrency[index].settings;
            made.lastPlayMixFrame = now;
        }

        JoinConcurrencyGroup(slot, group, now);
    }

    // 희생자를 멈춘다(eviction — 그룹에서 바로 빠진다, SC 960–986).
    for (uint32 at = 0; at < victimCount; ++at) {
        StealPlayback(victims[at], true, batch, now);
    }

    return ConcurrencyOutcome::Accepted;
}

bool AudioDeviceContext::CanPlayInGroup(uint32 group, uint32 slot, bool realization, uint64 now,
                                        uint32& victim) noexcept {
    ConcurrencyGroupState& state = groups_[group];
    const SoundConcurrencySettings& settings = state.settings;
    // 재시작 간격(SC 511–523): 통과하면 바로 지금으로 한다(앞에서 통과한 그룹만 — 거부에서 멈춘다).
    if (settings.retriggerTime > 0.0f &&
        now - state.lastPlayMixFrame < SecondsToMixFrames(settings.retriggerTime)) {
        return false;
    }

    state.lastPlayMixFrame = now;
    if (IsCullRule(settings.resolutionRule) || state.count < settings.maxCount) {
        return true;
    }

    const Record& incoming = records_[slot];
    GatherGroupMembers(group, slot);
    uint32 best = NumericLimits<uint32>::Max();
    switch (settings.resolutionRule) {
    case SoundConcurrencyRule::StopOldest: {
        // 재생 시간이 가장 긴 것(SC 743–762). 목록 앞부터 `>`로 보므로 같으면 앞(먼저 수락한)
        // 쪽이다.
        double oldest = -1.0;
        for (const uint32 other : groupScratch_) {
            if (records_[other].playbackTime > oldest) {
                oldest = records_[other].playbackTime;
                best = other;
            }
        }

        // 실현이면 새 재생이 더 오래됐을 때 빼앗지 않는다(SC 756).
        if (best != NumericLimits<uint32>::Max() && realization &&
            incoming.playbackTime > records_[best].playbackTime) {
            best = NumericLimits<uint32>::Max();
        }

        break;
    }
    case SoundConcurrencyRule::StopFarthestThenPreventNew:
    case SoundConcurrencyRule::StopFarthestThenOldest: {
        // UE GetEvictableSoundStopFarthest(SC 764–816) 그대로: 기준(처음은 새 재생 거리²)보다
        // delta² 넘게 먼 것을 만날 때마다 기준을 올린다. ThenOldest는 기준과 delta² 안(같은
        // 거리)이면 재생 시간이 더 긴 쪽을 고르고 기준을 그 거리로 한다(실현이면 새 재생보다 긴
        // 쪽만). 그래서 모두 더 가까우면 대상이 없고, 같은 거리(2D 포함)면 오래된 쪽이 희생자다.
        const double delta =
            static_cast<double>(minDistanceDelta_) * static_cast<double>(minDistanceDelta_);
        double threshold = ListenerDistanceSquared(incoming);
        for (const uint32 other : groupScratch_) {
            const Record& record = records_[other];
            const double distance = ListenerDistanceSquared(record);
            if (distance > threshold + delta) {
                threshold = distance;
                best = other;
                continue;
            }

            if (settings.resolutionRule == SoundConcurrencyRule::StopFarthestThenOldest &&
                math::Abs(distance - threshold) <= delta) {
                const bool older = best == NumericLimits<uint32>::Max() ||
                                   record.playbackTime > records_[best].playbackTime;
                if (older && (!realization || record.playbackTime > incoming.playbackTime)) {
                    threshold = distance;
                    best = other;
                }
            }
        }

        break;
    }
    case SoundConcurrencyRule::PreventNew:
    case SoundConcurrencyRule::StopLowestPriority:
    case SoundConcurrencyRule::StopQuietest:
    case SoundConcurrencyRule::StopLowestPriorityThenPreventNew:
        break;
    }

    if (best == NumericLimits<uint32>::Max()) {
        return false;
    }

    victim = best;
    return true;
}

void AudioDeviceContext::JoinConcurrencyGroup(uint32 slot, uint32 group, uint64 now) noexcept {
    Record& record = records_[slot];
    ConcurrencyGroupState& state = groups_[group];
    const SoundConcurrencySettings& settings = state.settings;
    const uint32 generation = state.count;
    // Default 순서: 앞 재생마다 목표 V^(새 세대 − 자기 세대)를 attack 동안(SC 914–943).
    // CanRelease면 목표를 올리는 갱신은 건너뛴다.
    if (settings.volumeScaleMode == SoundConcurrencyVolumeScaleMode::Default &&
        settings.volumeScale < 1.0f) {
        const uint32 attack = SecondsToMixFrames(settings.volumeScaleAttackTime);
        for (Record& other : records_) {
            for (uint32 index = 0; index < other.linkCount; ++index) {
                ConcurrencyLink& link = other.links[index];
                if (link.group != group) {
                    continue;
                }

                const float target = ScalePower(settings.volumeScale, generation - link.generation);
                if (settings.volumeScaleCanRelease && target > link.target) {
                    continue;
                }

                link.target = target;
                link.scale.StartTo(target, now, attack);
            }
        }
    }

    ROCKS_ASSERT(record.linkCount < MaxConcurrencyGroups);
    ConcurrencyLink& link = record.links[record.linkCount++];
    link = ConcurrencyLink{};
    link.group = group;
    link.generation = generation;
    link.target = 1.0f;
    link.scale.Set(1.0f, ConcurrencyScaleFloorDb);
    link.fresh = true;
    ++state.count;
}

void AudioDeviceContext::LeaveConcurrencyGroups(uint32 slot, uint64 now) noexcept {
    Record& record = records_[slot];
    for (uint32 index = 0; index < record.linkCount; ++index) {
        const uint32 group = record.links[index].group;
        const uint32 removed = record.links[index].generation;
        ConcurrencyGroupState& state = groups_[group];
        ROCKS_ASSERT(state.count > 0);
        --state.count;
        const SoundConcurrencySettings& settings = state.settings;
        const bool release = settings.volumeScaleCanRelease &&
                             settings.volumeScaleMode == SoundConcurrencyVolumeScaleMode::Default;
        const uint32 releaseFrames = SecondsToMixFrames(settings.volumeScaleReleaseTime);
        // 뒤 세대를 하나씩 내리고, CanRelease면 V^(수 − 세대 − 1)로 되돌린다(SC 315–346).
        for (uint32 other = 0; other < records_.Size(); ++other) {
            if (other == slot) {
                continue;
            }

            Record& member = records_[other];
            for (uint32 at = 0; at < member.linkCount; ++at) {
                ConcurrencyLink& link = member.links[at];
                if (link.group != group) {
                    continue;
                }

                if (link.generation > removed) {
                    --link.generation;
                }

                if (release) {
                    const uint32 rank = state.count - link.generation - 1;
                    link.target = ScalePower(settings.volumeScale, rank);
                    link.scale.StartTo(link.target, now, releaseFrames);
                }
            }
        }
    }

    record.linkCount = 0;
}

void AudioDeviceContext::StealPlayback(uint32 slot, bool evict, ControlBatch& batch,
                                       uint64 now) noexcept {
    Record& record = records_[slot];
    // fade = 속한 그룹 voiceStealReleaseTime의 최소값(AS 825–856). cull 규칙 그룹이 있고 loop이고
    // 소리를 낸 시간(UE PlaybackTimeNonVirtualized)이 0.1 s 안쪽이면 fade 없이 멈춘다.
    float release = MaxVoiceStealReleaseSeconds;
    bool cullGroup = false;
    for (uint32 index = 0; index < record.linkCount; ++index) {
        const SoundConcurrencySettings& settings = groups_[record.links[index].group].settings;
        release = math::Min(release, settings.voiceStealReleaseTime);
        cullGroup = cullGroup || IsCullRule(settings.resolutionRule);
    }

    // eviction은 그룹에서 바로 뺀다(뒤 평가에 세지 않게). 이미 정지·steal fade 중이면 그것으로
    // 끝이다(UE CreateAndEvictActiveSounds — FadeOut Concurrency·IsPendingStop이면 건드리지 않음).
    if (evict) {
        LeaveConcurrencyGroups(slot, now);
    }

    if (record.stopRequested) {
        return;
    }

    ++stats_.concurrencySteals;
    const SoundPlayable& playable = *record.sound.Get();
    const bool barelyPlayed =
        record.voicedTime < StealImmediateSeconds * static_cast<double>(mixer_.Config().sampleRate);
    uint32 fade = SecondsToMixFrames(release);
    if (cullGroup && playable.looping && barelyPlayed) {
        fade = 0;
    }

    // 가상화 가능한 loop는 끝내지 않고 거리 검사 없이 가상으로 둔다(SC 1085–1095). voice는 steal
    // fade를 분리 목록에서 마친다(UE도 원래 sound를 fade로 멈추고 가상 사본을 만든다 — SC
    // 1081–1100).
    if (IsVirtualizable(record) && record.state != RecordState::Virtual) {
        Virtualize(slot, batch, now, fade);
        return;
    }

    switch (record.state) {
    case RecordState::Voiceless:
    case RecordState::Virtual:
        // voice가 없는 희생자는 바로 끝낸다.
        EndVoiceless(slot, PlaybackEndReason::Stolen, batch);
        return;
    case RecordState::Deferred:
        ReleaseDeferredVoice(slot, now);
        EndVoiceless(slot, PlaybackEndReason::Stolen, batch);
        return;
    case RecordState::Submitted:
        break;
    case RecordState::Inactive:
        return;
    }

    if (fade == 0) {
        RequestStop(record, audio::DefaultStopFadeFrames, PlaybackEndReason::Stolen);
        return;
    }

    // steal fader: 0 dB → −80 dB를 dB 선형으로(UE FVolumeFader Logarithmic), 끝나면 믹서 정지 fade.
    record.stopRequested = true;
    record.stopReason = PlaybackEndReason::Stolen;
    record.stealing = true;
    record.stealFader.Set(1.0f, StealFadeFloorDb);
    record.stealFader.StartTo(0.0f, now, fade);
    record.stealFadeEnd = now + fade;
    record.paramsDirty = true;
}

void AudioDeviceContext::UpdateConcurrency(ControlBatch& batch, uint64 now) noexcept {
    // 떼어 낸 voice의 steal fade: fader를 곱해 게시하고, 끝나면 믹서 정지 fade다.
    for (DetachedVoice& detached : detached_) {
        if (!detached.fading) {
            continue;
        }

        if (now >= detached.fadeEnd) {
            // 정지 꼬리 동안 일시정지 전환이 다시 낼 값은 fader 끝값을 곱한 것이다.
            detached.params.gain *= detached.fader.At(now);
            detached.fading = false;
            (void)mixer_.RequestStop(detached.voice, audio::DefaultStopFadeFrames);
            continue;
        }

        audio::AudioVoiceParams params = detached.params;
        params.gain = detached.params.gain * detached.fader.At(now);
        (void)mixer_.PublishParams(detached.voice, params);
    }

    // steal fader가 끝난 재생은 믹서 정지 fade로 넘긴다.
    for (Record& record : records_) {
        if (record.stealing && now >= record.stealFadeEnd) {
            record.stealing = false;
            (void)mixer_.RequestStop(record.voice, audio::DefaultStopFadeFrames);
            record.stopFade = audio::DefaultStopFadeFrames < record.stopFade
                                  ? audio::DefaultStopFadeFrames
                                  : record.stopFade;
        }
    }

    uint32 used = 0;
    for (uint32 group = 0; group < groups_.Size(); ++group) {
        ConcurrencyGroupState& state = groups_[group];
        if (!state.used) {
            continue;
        }

        if (state.count == 0) {
            // 빈 그룹은 지운다. 재시작 간격이 있으면 간격이 지날 때까지 남긴다(마지막 재생 시각).
            const SoundConcurrencySettings& settings = state.settings;
            if (settings.retriggerTime <= 0.0f ||
                now - state.lastPlayMixFrame >= SecondsToMixFrames(settings.retriggerTime)) {
                state = ConcurrencyGroupState{};
                continue;
            }
        }

        ++used;
        if (IsCullRule(state.settings.resolutionRule)) {
            CullConcurrencyGroup(group, batch, now);
        }

        if (state.settings.volumeScaleMode != SoundConcurrencyVolumeScaleMode::Default) {
            RankConcurrencyGroup(group, now);
        }
    }

    stats_.concurrencyGroups = used;
}

void AudioDeviceContext::CullConcurrencyGroup(uint32 group, ControlBatch& batch,
                                              uint64 now) noexcept {
    // UE CullSoundsDueToMaxConcurrency(SC 428–509): 멤버(정지 중인 것 포함 — UE는 그룹의 모든
    // ActiveSound를 센다)가 maxCount를 넘으면 기준 오름차순 앞의 (수 − maxCount)개를 빼앗는다. 이미
    // 정지 중인 것은 그대로 둔다. 희생자는 terminal까지 그룹에 남는다.
    const ConcurrencyGroupState& state = groups_[group];
    if (state.count <= state.settings.maxCount) {
        return;
    }

    GatherGroupMembers(group, NumericLimits<uint32>::Max());
    const SoundConcurrencyRule rule = state.settings.resolutionRule;
    const Array<Record>& records = records_;
    algo::Sort(
        Span<uint32>(groupScratch_.Data(), groupScratch_.Size()),
        [&](uint32 left, uint32 right) noexcept {
            const Record& a = records[left];
            const Record& b = records[right];
            if (rule == SoundConcurrencyRule::StopQuietest) {
                // 동률이면 새 쪽이 먼저 멈춘다(SC 452–460).
                if (!NearlyEqual(a.unscaledEstimate, b.unscaledEstimate, ConcurrencyTieTolerance)) {
                    return a.unscaledEstimate < b.unscaledEstimate;
                }

                return a.sequence > b.sequence;
            }

            if (!NearlyEqual(ConcurrencyPriorityOf(a), ConcurrencyPriorityOf(b),
                             ConcurrencyTieTolerance)) {
                return ConcurrencyPriorityOf(a) < ConcurrencyPriorityOf(b);
            }

            // StopLowestPriority는 오래된 쪽이, ThenPreventNew는 새 쪽이 먼저 멈춘다(SC 462–477).
            return rule == SoundConcurrencyRule::StopLowestPriority ? a.sequence < b.sequence
                                                                    : a.sequence > b.sequence;
        });

    const usize excess = groupScratch_.Size() > state.settings.maxCount
                             ? groupScratch_.Size() - state.settings.maxCount
                             : 0;
    for (usize index = 0; index < excess; ++index) {
        const uint32 slot = groupScratch_[index];
        if (!records_[slot].stopRequested) {
            StealPlayback(slot, false, batch, now);
        }
    }
}

void AudioDeviceContext::RankConcurrencyGroup(uint32 group, uint64 now) noexcept {
    // Distance·Priority 순서(SC 348–426): 먼 것·낮은 우선순위가 세대 0, 동률은 오래된 쪽이 앞(UE
    // 코드 그대로). 목표 V^(수 − 세대 − 1). 이번 묶음에 들어온 재생은 바로, 나머지는 attack 동안.
    const ConcurrencyGroupState& state = groups_[group];
    GatherGroupMembers(group, NumericLimits<uint32>::Max());
    const bool byDistance =
        state.settings.volumeScaleMode == SoundConcurrencyVolumeScaleMode::Distance;
    algo::Sort(Span<uint32>(groupScratch_.Data(), groupScratch_.Size()),
               [&](uint32 left, uint32 right) noexcept {
                   const Record& a = records_[left];
                   const Record& b = records_[right];
                   if (byDistance) {
                       const double da = ListenerDistanceSquared(a);
                       const double db = ListenerDistanceSquared(b);
                       if (!NearlyEqual(da, db, ConcurrencyTieTolerance)) {
                           return da > db;
                       }
                   } else if (!NearlyEqual(ConcurrencyPriorityOf(a), ConcurrencyPriorityOf(b),
                                           ConcurrencyTieTolerance)) {
                       return ConcurrencyPriorityOf(a) < ConcurrencyPriorityOf(b);
                   }

                   return a.sequence < b.sequence;
               });

    const uint32 count = static_cast<uint32>(groupScratch_.Size());
    const uint32 attack = SecondsToMixFrames(state.settings.volumeScaleAttackTime);
    for (uint32 rank = 0; rank < count; ++rank) {
        Record& record = records_[groupScratch_[rank]];
        for (uint32 index = 0; index < record.linkCount; ++index) {
            ConcurrencyLink& link = record.links[index];
            if (link.group != group) {
                continue;
            }

            link.generation = rank;
            const float target = ScalePower(state.settings.volumeScale, count - rank - 1);
            if (link.fresh) {
                link.fresh = false;
                link.target = target;
                link.scale.Set(target, ConcurrencyScaleFloorDb);
            } else if (target != link.target) {
                link.target = target;
                link.scale.StartTo(target, now, attack);
            }
        }
    }
}

bool AudioDeviceContext::ConcurrencyRamping(const Record& record, uint64 now) const noexcept {
    if (record.stealing) {
        return true;
    }

    for (uint32 index = 0; index < record.linkCount; ++index) {
        if (record.links[index].scale.IsRamping(now)) {
            return true;
        }
    }

    return false;
}

} // namespace rocks
