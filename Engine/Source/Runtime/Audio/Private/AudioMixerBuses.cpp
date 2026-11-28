#include "AudioMixerInternal.h"

#include <Audio/AudioMixer.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Atomics.h>

// 믹서 버스 graph·plan·voice 라우팅이다(Docs/Design/AudioSystem 3.45절, UE FMixerSubmix·
// FMixerSourceSubmixOutputBuffer 대응). 제어 쪽(버스 칸·plan 게시·라우팅 출판)과 RT 쪽(plan
// 채택·send 혼합·버스 처리)이 함께 있다. 스레드 경계는 AudioMixer.h 머리 주석과 같다.
namespace rocks::audio {

namespace {

// plan word의 버퍼 색인 mask다.
constexpr uint64 BusPlanBufferMask = (uint64{1} << mixer_internal::PlanWordBufferBits) - 1;

// plan 게시 scratch에서 links에 없는 칸이다.
constexpr uint32 BusNoLink = NumericLimits<uint32>::Max();

// 라우팅 send word의 표지 bit다(단계·LPF·HPF).
constexpr uint32 RouteFlagPre = 1u << 0;
constexpr uint32 RouteFlagLowPass = 1u << 1;
constexpr uint32 RouteFlagHighPass = 1u << 2;

// 라우팅 seqlock 머리 word 색인이다.
enum RouteHeaderWord : uint32 {
    RouteHeaderGeneration,
    RouteHeaderSequenceLow,
    RouteHeaderSequenceHigh,
    RouteHeaderSendCount,
    RouteHeaderWordCount
};

// 라우팅 seqlock send 칸 안 word 색인이다.
enum RouteSendWord : uint32 {
    RouteSendBusIndex,
    RouteSendBusGeneration,
    RouteSendLevel,
    RouteSendFlags,
    RouteSendLowPass,
    RouteSendHighPass,
    RouteSendWordCount
};

// 버스 seqlock word 색인이다.
enum BusSeqlockWord : uint32 {
    BusSeqlockGeneration,
    BusSeqlockOutput,
    BusSeqlockWet,
    BusSeqlockDry,
    BusSeqlockWordCount
};

bool IsValidBusGain(float gain) noexcept {
    return math::IsFinite(gain) && gain >= 0.0f && gain <= MaxBusGain;
}

bool IsValidBusParams(const AudioBusParams& params) noexcept {
    return IsValidBusGain(params.outputGain) && IsValidBusGain(params.wetGain) &&
           IsValidBusGain(params.dryGain);
}

bool IsValidSendCutoff(float hz) noexcept {
    return math::IsFinite(hz) && hz >= MinSendFilterHz && hz <= MaxSendFilterHz;
}

bool SameBus(AudioBusHandle a, AudioBusHandle b) noexcept {
    return a.index == b.index && a.generation == b.generation;
}

} // namespace

using mixer_internal::BitsFloat;
using mixer_internal::ClampCounted;
using mixer_internal::CopyView;
using mixer_internal::FloatBits;
using mixer_internal::HighWordShift;
using mixer_internal::LowWordMask;
using mixer_internal::NextGeneration;
using mixer_internal::PlanBufferCount;
using mixer_internal::PlanWordBufferBits;
using mixer_internal::SeqlockWritingBit;
using platform::AtomicLoad;
using platform::AtomicStore;
using platform::AtomicThreadFence;
using platform::MemoryOrder;

static_assert(PlanBufferCount <= BusPlanBufferMask + 1,
              "Plan word buffer bits must index every plan buffer");

AudioVoiceRoute AudioMixer::MasterRoute() noexcept {
    AudioVoiceRoute route;
    route.sendCount = 1;
    route.sends[0].bus = MasterBus();
    return route;
}

// --- 제어 쪽: 라우팅 ---

Result<void> AudioMixer::ValidateRoute(const AudioVoiceRoute& route) const noexcept {
    if (route.sendCount > MaxVoiceSends) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < route.sendCount; ++index) {
        const AudioVoiceSend& send = route.sends[index];
        // 가장 최근에 게시한 plan에 있는 버스만 받는다(3.45.4절 — RT가 낡은 send를 보지 않게).
        if (send.bus.index >= controlBuses_.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const ControlBus& bus = controlBuses_[send.bus.index];
        const bool validStage =
            static_cast<uint32>(send.stage) <= static_cast<uint32>(AudioSendStage::PreAttenuation);
        if (!bus.acquired || !bus.inLatestPlan || bus.generation != send.bus.generation ||
            !math::IsFinite(send.level) || send.level < 0.0f || send.level > MaxVoiceGain ||
            !validStage || (send.lowPassEnabled && !IsValidSendCutoff(send.lowPassCutoffHz)) ||
            (send.highPassEnabled && !IsValidSendCutoff(send.highPassCutoffHz))) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (uint32 earlier = 0; earlier < index; ++earlier) {
            if (route.sends[earlier].bus.index == send.bus.index) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> AudioMixer::PublishRoute(AudioVoiceHandle voice,
                                      const AudioVoiceRoute& route) noexcept {
    if (auto checked = CheckCommandTarget(voice); !checked) {
        return checked;
    }

    if (auto validated = ValidateRoute(route); !validated) {
        return validated;
    }

    WriteRouteSeqlock(voice, route);
    return Result<void>::Success();
}

void AudioMixer::WriteRouteSeqlock(AudioVoiceHandle voice, const AudioVoiceRoute& route) noexcept {
    static_assert(RouteHeaderWords == RouteHeaderWordCount,
                  "Route seqlock header must match the RouteHeaderWord layout");
    static_assert(RouteSendWords == RouteSendWordCount,
                  "Route seqlock send entries must match the RouteSendWord layout");
    // seqlock 쓰기: version을 홀수로 → release fence → 내용 → version을 짝수로(release).
    SharedVoice& shared = sharedVoices_[voice.index];
    const uint64 sequence = ++nextSequence_;
    uint32 words[RouteWords] = {};
    words[RouteHeaderGeneration] = voice.generation;
    words[RouteHeaderSequenceLow] = static_cast<uint32>(sequence & LowWordMask);
    words[RouteHeaderSequenceHigh] = static_cast<uint32>(sequence >> HighWordShift);
    words[RouteHeaderSendCount] = route.sendCount;
    for (uint32 index = 0; index < route.sendCount; ++index) {
        const AudioVoiceSend& send = route.sends[index];
        uint32* entry = words + RouteHeaderWords + index * RouteSendWords;
        entry[RouteSendBusIndex] = send.bus.index;
        entry[RouteSendBusGeneration] = send.bus.generation;
        entry[RouteSendLevel] = FloatBits(send.level);
        entry[RouteSendFlags] = (send.stage == AudioSendStage::PreAttenuation ? RouteFlagPre : 0u) |
                                (send.lowPassEnabled ? RouteFlagLowPass : 0u) |
                                (send.highPassEnabled ? RouteFlagHighPass : 0u);
        entry[RouteSendLowPass] = FloatBits(send.lowPassCutoffHz);
        entry[RouteSendHighPass] = FloatBits(send.highPassCutoffHz);
    }

    const uint32 version = AtomicLoad<MemoryOrder::Relaxed>(&shared.routeVersion);
    AtomicStore<MemoryOrder::Relaxed>(&shared.routeVersion, version + 1);
    AtomicThreadFence<MemoryOrder::Release>();
    for (uint32 word = 0; word < RouteWords; ++word) {
        AtomicStore<MemoryOrder::Relaxed>(&shared.routeContents[word], words[word]);
    }

    AtomicStore<MemoryOrder::Release>(&shared.routeVersion, version + 2);
}

// --- 제어 쪽: 버스 칸·plan ---

Result<AudioBusHandle> AudioMixer::AcquireBus() noexcept {
    for (uint32 index = MasterBusIndex + 1; index < controlBuses_.Size(); ++index) {
        ControlBus& bus = controlBuses_[index];
        if (!bus.acquired) {
            bus.acquired = true;
            bus.inLatestPlan = false;
            bus.removedAtSerial = 0;
            return Result<AudioBusHandle>::Success(AudioBusHandle{index, bus.generation});
        }
    }

    return Result<AudioBusHandle>::Failure(ErrorCode::CapacityExceeded);
}

Result<void> AudioMixer::ReleaseBus(AudioBusHandle bus) noexcept {
    if (bus.index == MasterBusIndex) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (bus.index >= controlBuses_.Size() || !controlBuses_[bus.index].acquired ||
        controlBuses_[bus.index].generation != bus.generation) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // RT가 이 칸이 든 plan을 아직 쓸 수 있으면 놓지 않는다(3.45.4절 해제).
    ControlBus& slot = controlBuses_[bus.index];
    if (slot.inLatestPlan ||
        (slot.removedAtSerial != 0 && AdoptedPlanSerial() < slot.removedAtSerial)) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    slot.acquired = false;
    slot.inLatestPlan = false;
    slot.removedAtSerial = 0;
    slot.generation = NextGeneration(slot.generation);
    return Result<void>::Success();
}

Result<void> AudioMixer::PublishBusParams(AudioBusHandle bus,
                                          const AudioBusParams& params) noexcept {
    if (bus.index >= controlBuses_.Size() || !controlBuses_[bus.index].acquired ||
        controlBuses_[bus.index].generation != bus.generation) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (!IsValidBusParams(params)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    static_assert(BusSeqlockWords == BusSeqlockWordCount,
                  "Bus seqlock contents must match the BusSeqlockWord layout");
    SharedBus& shared = sharedBuses_[bus.index];
    const uint32 words[BusSeqlockWords] = {bus.generation, FloatBits(params.outputGain),
                                           FloatBits(params.wetGain), FloatBits(params.dryGain)};
    const uint32 version = AtomicLoad<MemoryOrder::Relaxed>(&shared.version);
    AtomicStore<MemoryOrder::Relaxed>(&shared.version, version + 1);
    AtomicThreadFence<MemoryOrder::Release>();
    for (uint32 word = 0; word < BusSeqlockWords; ++word) {
        AtomicStore<MemoryOrder::Relaxed>(&shared.contents[word], words[word]);
    }

    AtomicStore<MemoryOrder::Release>(&shared.version, version + 2);
    return Result<void>::Success();
}

uint64 AudioMixer::AdoptedPlanSerial() const noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&adoptedPlanSerial_);
}

Result<void> AudioMixer::PublishBusPlan(Span<const AudioBusLink> links) noexcept {
    const uint32 slots = static_cast<uint32>(controlBuses_.Size());
    if (links.Size() >= slots) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 칸 검사와 칸 → links 색인.
    for (uint32 slot = 0; slot < slots; ++slot) {
        planLinkOf_[slot] = BusNoLink;
    }

    for (uint32 index = 0; index < links.Size(); ++index) {
        const AudioBusLink& link = links[index];
        if (link.bus.index == MasterBusIndex || link.bus.index >= slots ||
            planLinkOf_[link.bus.index] != BusNoLink || !IsValidBusParams(link.initialParams)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 잡지 않은 칸과 이전 generation은 PublishBusParams·ReleaseBus처럼 StaleHandle이다.
        if (!controlBuses_[link.bus.index].acquired ||
            controlBuses_[link.bus.index].generation != link.bus.generation) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        planLinkOf_[link.bus.index] = index;
    }

    // 부모 검사와 깊이(Master까지 단계 수). 단계가 links 수를 넘으면 순환이다.
    const uint32 count = static_cast<uint32>(links.Size());
    uint32 maxDepth = 0;
    for (uint32 index = 0; index < count; ++index) {
        uint32 depth = 0;
        AudioBusHandle parent = links[index].parent;
        while (!SameBus(parent, MasterBus())) {
            if (parent.index >= slots || planLinkOf_[parent.index] == BusNoLink ||
                links[planLinkOf_[parent.index]].bus.generation != parent.generation ||
                depth >= count) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            parent = links[planLinkOf_[parent.index]].parent;
            ++depth;
        }

        planDepth_[links[index].bus.index] = depth + 1;
        maxDepth = depth + 1 > maxDepth ? depth + 1 : maxDepth;
    }

    // 미채택 plan 수와 빈 버퍼(RT가 채택한 것보다 오래된 일련번호).
    const uint64 adopted = AdoptedPlanSerial();
    uint32 pending = 0;
    uint32 freeBuffer = PlanBufferCount;
    for (uint32 buffer = 0; buffer < PlanBufferCount; ++buffer) {
        if (plans_[buffer].serial > adopted) {
            ++pending;
        } else if (plans_[buffer].serial < adopted && freeBuffer == PlanBufferCount) {
            freeBuffer = buffer;
        }
    }

    if (pending >= MaxPendingPlans) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    ROCKS_ASSERT(freeBuffer != PlanBufferCount);
    BusPlan& plan = plans_[freeBuffer];
    for (uint32 slot = 0; slot < slots; ++slot) {
        plan.generation[slot] = 0;
    }

    plan.generation[MasterBusIndex] = MasterBusGeneration;
    for (const AudioBusLink& link : links) {
        plan.generation[link.bus.index] = link.bus.generation;
        plan.parent[link.bus.index] = link.parent.index;
        plan.initial[link.bus.index] = link.initialParams;
    }

    // 깊은 칸(자식)부터 처리하고 Master가 마지막이다.
    uint32 written = 0;
    for (uint32 depth = maxDepth; depth > 0; --depth) {
        for (const AudioBusLink& link : links) {
            if (planDepth_[link.bus.index] == depth) {
                plan.order[written++] = link.bus.index;
            }
        }
    }

    plan.order[written++] = MasterBusIndex;
    plan.orderCount = written;
    const uint64 serial = ++publishedPlanSerial_;
    plan.serial = serial;
    for (uint32 slot = MasterBusIndex + 1; slot < slots; ++slot) {
        ControlBus& bus = controlBuses_[slot];
        const bool inPlan = planLinkOf_[slot] != BusNoLink;
        if (bus.inLatestPlan && !inPlan) {
            bus.removedAtSerial = serial;
        }

        bus.inLatestPlan = inPlan;
    }

    AtomicStore<MemoryOrder::Release>(&planWord_, (serial << PlanWordBufferBits) | freeBuffer);
    return Result<void>::Success();
}

// --- RT 쪽: 라우팅 ---

bool AudioMixer::ReadRouteSeqlock(uint32 index, uint32& version, uint32& generation,
                                  uint64& sequence, AudioVoiceRoute& route) noexcept {
    // seqlock 읽기: version(acquire) → 내용(relaxed) → acquire fence → version 재확인. 홀수·
    // 불일치면 건너뛰고 다음 블록에 다시 본다(대기 금지).
    const SharedVoice& shared = sharedVoices_[index];
    const uint32 before = AtomicLoad<MemoryOrder::Acquire>(&shared.routeVersion);
    if ((before & SeqlockWritingBit) != 0) {
        ++counters_.skippedSeqlockReads;
        return false;
    }

    if (before == 0) {
        return false;
    }

    uint32 words[RouteWords] = {};
    for (uint32 word = 0; word < RouteWords; ++word) {
        words[word] = AtomicLoad<MemoryOrder::Relaxed>(&shared.routeContents[word]);
    }

    if (seqlockReadHook_ != nullptr) {
        seqlockReadHook_(*this, index);
    }

    AtomicThreadFence<MemoryOrder::Acquire>();
    if (AtomicLoad<MemoryOrder::Relaxed>(&shared.routeVersion) != before) {
        ++counters_.skippedSeqlockReads;
        return false;
    }

    version = before;
    generation = words[RouteHeaderGeneration];
    sequence = (static_cast<uint64>(words[RouteHeaderSequenceHigh]) << HighWordShift) |
               words[RouteHeaderSequenceLow];
    const uint32 count = words[RouteHeaderSendCount];
    route.sendCount = count < MaxVoiceSends ? count : MaxVoiceSends;
    for (uint32 send = 0; send < route.sendCount; ++send) {
        const uint32* entry = words + RouteHeaderWords + send * RouteSendWords;
        AudioVoiceSend& target = route.sends[send];
        target.bus = AudioBusHandle{entry[RouteSendBusIndex], entry[RouteSendBusGeneration]};
        target.level = BitsFloat(entry[RouteSendLevel]);
        target.stage = (entry[RouteSendFlags] & RouteFlagPre) != 0
                           ? AudioSendStage::PreAttenuation
                           : AudioSendStage::PostAttenuation;
        target.lowPassEnabled = (entry[RouteSendFlags] & RouteFlagLowPass) != 0;
        target.highPassEnabled = (entry[RouteSendFlags] & RouteFlagHighPass) != 0;
        target.lowPassCutoffHz = BitsFloat(entry[RouteSendLowPass]);
        target.highPassCutoffHz = BitsFloat(entry[RouteSendHighPass]);
    }

    return true;
}

bool AudioMixer::IsSilentState(const VoiceSlot& slot) const noexcept {
    return slot.pauseState == PauseState::Paused || slot.startDeferred || slot.awaitingStream;
}

void AudioMixer::ApplyRouteSeqlock(uint32 index, VoiceSlot& slot) noexcept {
    // version이 그대로면 내용을 읽지 않는다(voice마다 블록마다 읽는 비용을 피한다).
    const uint32 current = AtomicLoad<MemoryOrder::Relaxed>(&sharedVoices_[index].routeVersion);
    if (current == slot.lastRouteVersion) {
        return;
    }

    uint32 version = 0;
    uint32 generation = 0;
    uint64 sequence = 0;
    AudioVoiceRoute route;
    if (!ReadRouteSeqlock(index, version, generation, sequence, route)) {
        return;
    }

    slot.lastRouteVersion = version;
    if (generation != slot.generation || sequence <= slot.lastRouteSequence) {
        return;
    }

    slot.lastRouteSequence = sequence;
    ApplyRoute(slot, route, IsSilentState(slot));
}

void AudioMixer::StartRoute(uint32 index, VoiceSlot& slot, uint64 playSequence) noexcept {
    for (SendSlot& send : slot.sends) {
        send.used = false;
        send.active = false;
    }

    // Play보다 뒤 순번으로 출판한 라우팅이 이긴다. 못 읽으면(찢어짐) 시작 라우팅을 쓰고 다음
    // 블록에 다시 본다(3.45.5절).
    AudioVoiceRoute route = sharedVoices_[index].startRoute;
    uint64 sequence = playSequence;
    uint32 version = 0;
    uint32 generation = 0;
    uint64 published = 0;
    AudioVoiceRoute latest;
    if (ReadRouteSeqlock(index, version, generation, published, latest)) {
        slot.lastRouteVersion = version;
        if (generation == slot.generation && published > playSequence) {
            route = latest;
            sequence = published;
        }
    }

    slot.lastRouteSequence = sequence;
    ApplyRoute(slot, route, true);
}

void AudioMixer::ApplyRoute(VoiceSlot& slot, const AudioVoiceRoute& route,
                            bool immediate) noexcept {
    uint64& clamped = counters_.clampedParams;
    const uint32 outputs = OutputChannelCount();
    const uint32 ramp = immediate ? 0u : DefaultParamRampFrames;
    // 이번 라우팅 전에 빠지는 중이던 칸(넘칠 때 끊을 후보)이다.
    bool wasRemoving[MaxVoiceSendSlots] = {};
    bool wasActive[MaxVoiceSendSlots] = {};
    for (uint32 index = 0; index < MaxVoiceSendSlots; ++index) {
        SendSlot& send = slot.sends[index];
        wasRemoving[index] = send.used && !send.active;
        wasActive[index] = send.used && send.active;
        send.active = false;
    }

    const uint32 count = route.sendCount < MaxVoiceSends ? route.sendCount : MaxVoiceSends;
    for (uint32 entry = 0; entry < count; ++entry) {
        const AudioVoiceSend& want = route.sends[entry];
        const float level = ClampCounted(want.level, 0.0f, MaxVoiceGain, 0.0f, clamped);
        SendSlot* target = nullptr;
        for (SendSlot& send : slot.sends) {
            if (send.used && SameBus(send.bus, want.bus)) {
                target = &send;
                break;
            }
        }

        if (target == nullptr) {
            for (SendSlot& send : slot.sends) {
                if (!send.used) {
                    target = &send;
                    break;
                }
            }
        }

        if (target == nullptr) {
            // 빠지는 중인 칸 가운데 가장 오래된 것을 끊는다(빠지는 칸이 없으면 쓰는 칸이
            // MaxVoiceSends 이하라 빈 칸이 있다).
            uint32 oldest = MaxVoiceSendSlots;
            for (uint32 index = 0; index < MaxVoiceSendSlots; ++index) {
                if (wasRemoving[index] && !slot.sends[index].active &&
                    (oldest == MaxVoiceSendSlots ||
                     slot.sends[index].removalSerial < slot.sends[oldest].removalSerial)) {
                    oldest = index;
                }
            }

            ROCKS_ASSERT(oldest != MaxVoiceSendSlots);
            ++counters_.cutBusSends;
            wasRemoving[oldest] = false;
            target = &slot.sends[oldest];
            target->used = false;
        }

        if (!target->used) {
            // 새 칸: level은 0에서 시작하고 필터 상태를 새로 만든다.
            target->used = true;
            target->bus = want.bus;
            target->level.Reset(0.0f);
            target->lowPassEnabled = false;
            target->highPassEnabled = false;
        }

        target->active = true;
        target->stage =
            static_cast<uint32>(want.stage) <= static_cast<uint32>(AudioSendStage::PreAttenuation)
                ? want.stage
                : AudioSendStage::PostAttenuation;
        // 0에서 0으로 가는 ramp는 두지 않는다(무음을 섞어 버스를 깨우지 않게).
        const bool silentToSilent = target->level.Current() == 0.0f && level == 0.0f;
        if (immediate || silentToSilent) {
            target->level.Reset(level);
        } else {
            target->level.SetTarget(level, ramp);
        }

        if (want.lowPassEnabled) {
            const float hz = ClampCounted(want.lowPassCutoffHz, MinSendFilterHz, MaxSendFilterHz,
                                          MaxSendFilterHz, clamped);
            if (!target->lowPassEnabled) {
                // 켤 때 상태를 초기화한다(UE Init). 인자는 늘 유효하다.
                auto created = OnePoleLowPass::TryCreate(config_.sampleRate, outputs);
                ROCKS_ASSERT(created);
                target->lowPass = created.Value();
                (void)target->lowPass.SetCutoff(hz, 0);
            } else {
                (void)target->lowPass.SetCutoff(hz, ramp);
            }
        }

        if (want.highPassEnabled) {
            const float hz = ClampCounted(want.highPassCutoffHz, MinSendFilterHz, MaxSendFilterHz,
                                          MinSendFilterHz, clamped);
            if (!target->highPassEnabled) {
                auto created = OnePoleHighPass::TryCreate(config_.sampleRate, outputs);
                ROCKS_ASSERT(created);
                target->highPass = created.Value();
                (void)target->highPass.SetCutoff(hz, 0);
            } else {
                (void)target->highPass.SetCutoff(hz, ramp);
            }
        }

        target->lowPassEnabled = want.lowPassEnabled;
        target->highPassEnabled = want.highPassEnabled;
    }

    // 빠진 send: 소리를 내지 않는 상태면 바로 비우고, 아니면 0으로 ramp한 뒤 비운다.
    for (uint32 index = 0; index < MaxVoiceSendSlots; ++index) {
        SendSlot& send = slot.sends[index];
        if (!send.used || send.active) {
            continue;
        }

        if (immediate || (!send.level.IsRamping() && send.level.Current() == 0.0f)) {
            send.used = false;
        } else if (wasActive[index]) {
            send.removalSerial = ++nextRemovalSerial_;
            send.level.SetTarget(0.0f, DefaultParamRampFrames);
        }
    }
}

bool AudioMixer::HasPreSend(const VoiceSlot& slot) const noexcept {
    for (const SendSlot& send : slot.sends) {
        if (send.used && send.stage == AudioSendStage::PreAttenuation &&
            (send.level.IsRamping() || send.level.Current() != 0.0f)) {
            return true;
        }
    }

    return false;
}

void AudioMixer::MixSends(VoiceSlot& slot, const PlanarView& post, const PlanarView* pre,
                          uint32 offset, uint32 frames) noexcept {
    const uint32 outputs = OutputChannelCount();
    for (SendSlot& send : slot.sends) {
        if (!send.used || (!send.level.IsRamping() && send.level.Current() == 0.0f)) {
            continue;
        }

        if (send.bus.index >= rtBuses_.Size() ||
            rtBuses_[send.bus.index].generation != send.bus.generation) {
            // 채택한 plan에 없는 버스다(계약 위반 방어 — 3.45.4절).
            ++counters_.staleBusSends;
            send.level.Advance(frames);
            continue;
        }

        const bool usePre = send.stage == AudioSendStage::PreAttenuation;
        ROCKS_ASSERT(!usePre || pre != nullptr);
        PlanarView source = usePre ? *pre : post;
        if (send.lowPassEnabled || send.highPassEnabled) {
            const PlanarView filtered = sendScratch_.View(frames);
            CopyView(filtered, source);
            if (send.lowPassEnabled) {
                send.lowPass.Process(filtered);
            }

            if (send.highPassEnabled) {
                send.highPass.Process(filtered);
            }

            source = filtered;
        }

        const PlanarView destination = BusInput(send.bus.index);
        for (uint32 output = 0; output < outputs; ++output) {
            GainRamp level = send.level;
            level.MixAdd(source.channels[output], destination.channels[output] + offset, frames);
        }

        send.level.Advance(frames);
        rtBuses_[send.bus.index].touched = true;
    }
}

void AudioMixer::AdvanceRemovingSends(VoiceSlot& slot, uint32 frames) noexcept {
    for (SendSlot& send : slot.sends) {
        if (send.used && !send.active) {
            send.level.Advance(frames);
        }
    }
}

void AudioMixer::FreeFinishedSends(VoiceSlot& slot, bool all) noexcept {
    for (SendSlot& send : slot.sends) {
        if (send.used && !send.active &&
            (all || (!send.level.IsRamping() && send.level.Current() == 0.0f))) {
            send.used = false;
        }
    }
}

// --- RT 쪽: plan·버스 ---

PlanarView AudioMixer::BusInput(uint32 index) noexcept {
    return index == MasterBusIndex ? master_.View() : busInputs_[index].View();
}

void AudioMixer::AdoptPlan() noexcept {
    // 지난 블록에 더해진 입력을 비운다(처리는 제자리라 출력이 남아 있다). Master는 늘 비운다.
    for (uint32 slot = 0; slot < rtBuses_.Size(); ++slot) {
        RtBus& bus = rtBuses_[slot];
        if (bus.touched && slot != MasterBusIndex) {
            busInputs_[slot].Clear();
        }

        bus.touched = false;
    }

    master_.Clear();
    const uint64 word = AtomicLoad<MemoryOrder::Acquire>(&planWord_);
    const uint64 serial = word >> PlanWordBufferBits;
    if (serial == plans_[rtPlan_].serial) {
        return;
    }

    rtPlan_ = static_cast<uint32>(word & BusPlanBufferMask);
    const BusPlan& plan = plans_[rtPlan_];
    uint64 active = 0;
    for (uint32 slot = 0; slot < rtBuses_.Size(); ++slot) {
        RtBus& bus = rtBuses_[slot];
        const uint32 generation = plan.generation[slot];
        if (generation != bus.generation) {
            // 새 generation은 plan의 첫 값으로 ramp 없이 시작한다(3.45.4절).
            bus.generation = generation;
            bus.lastVersion = 0;
            if (generation != 0) {
                const AudioBusParams& initial = plan.initial[slot];
                bus.output.Reset(initial.outputGain);
                bus.wet.Reset(initial.wetGain);
                bus.dry.Reset(initial.dryGain);
                bus.fresh = true;
            }
        }

        active += generation != 0 ? 1u : 0u;
    }

    counters_.activeBuses = active;
    AtomicStore<MemoryOrder::Release>(&adoptedPlanSerial_, serial);
}

void AudioMixer::ProcessBusSeqlocks() noexcept {
    uint64& clamped = counters_.clampedParams;
    const AudioBusParams defaults;
    for (uint32 slot = 0; slot < rtBuses_.Size(); ++slot) {
        RtBus& bus = rtBuses_[slot];
        if (bus.generation == 0) {
            bus.fresh = false;
            continue;
        }

        // fresh는 이 generation의 값을 읽었거나 읽을 값이 없을 때만 내린다(찢어진 읽기면 다음
        // 블록에 그 값을 ramp 없이 쓴다 — 3.45.4절).
        const bool fresh = bus.fresh;
        const SharedBus& shared = sharedBuses_[slot];
        const uint32 before = AtomicLoad<MemoryOrder::Acquire>(&shared.version);
        if (before == 0 || before == bus.lastVersion) {
            bus.fresh = false;
            continue;
        }

        if ((before & SeqlockWritingBit) != 0) {
            ++counters_.skippedSeqlockReads;
            continue;
        }

        uint32 words[BusSeqlockWords] = {};
        for (uint32 word = 0; word < BusSeqlockWords; ++word) {
            words[word] = AtomicLoad<MemoryOrder::Relaxed>(&shared.contents[word]);
        }

        AtomicThreadFence<MemoryOrder::Acquire>();
        if (AtomicLoad<MemoryOrder::Relaxed>(&shared.version) != before) {
            ++counters_.skippedSeqlockReads;
            continue;
        }

        bus.lastVersion = before;
        bus.fresh = false;
        if (words[BusSeqlockGeneration] != bus.generation) {
            continue;
        }

        const float output = ClampCounted(BitsFloat(words[BusSeqlockOutput]), 0.0f, MaxBusGain,
                                          defaults.outputGain, clamped);
        const float wet = ClampCounted(BitsFloat(words[BusSeqlockWet]), 0.0f, MaxBusGain,
                                       defaults.wetGain, clamped);
        const float dry = ClampCounted(BitsFloat(words[BusSeqlockDry]), 0.0f, MaxBusGain,
                                       defaults.dryGain, clamped);
        if (fresh) {
            bus.output.Reset(output);
            bus.wet.Reset(wet);
            bus.dry.Reset(dry);
        } else {
            // UE ArrayFade처럼 한 블록에 걸쳐 바꾼다.
            bus.output.SetTarget(output, config_.blockFrames);
            bus.wet.SetTarget(wet, config_.blockFrames);
            bus.dry.SetTarget(dry, config_.blockFrames);
        }
    }
}

void AudioMixer::ProcessBuses() noexcept {
    const BusPlan& plan = plans_[rtPlan_];
    const uint32 frames = config_.blockFrames;
    for (uint32 position = 0; position < plan.orderCount; ++position) {
        const uint32 slot = plan.order[position];
        RtBus& bus = rtBuses_[slot];
        if (!bus.touched) {
            // 입력이 없으면 출력도 0이다. ramp만 한 블록 진행한다(자동 비활성 꼬리는 AU7g).
            bus.output.Advance(frames);
            bus.wet.Advance(frames);
            bus.dry.Advance(frames);
            continue;
        }

        const PlanarView input = BusInput(slot);
        // dry 경로는 UE 조건(목표 ≠ 현재 또는 현재 ≠ 0, 허용치 1e−8)일 때만 쓴다(3.45.3절).
        const float dryNow = bus.dry.Current();
        const bool dryActive = math::Abs(bus.dry.Target() - dryNow) > NegligibleBusGain ||
                               math::Abs(dryNow) > NegligibleBusGain;
        const PlanarView dry = dryScratch_.View(frames);
        if (dryActive) {
            CopyView(dry, input);
            bus.dry.Apply(dry);
        } else {
            bus.dry.Advance(frames);
        }

        // wet 경로는 효과 체인(AU7g)이 생기기 전에는 입력 그대로이고 wet gain을 걸지 않는다(UE).
        bus.wet.Advance(frames);
        if (dryActive) {
            MixAdd(input, dry, 1.0f);
        }

        bus.output.Apply(input);
        if (slot != MasterBusIndex) {
            const uint32 parent = plan.parent[slot];
            MixAdd(BusInput(parent), input, 1.0f);
            rtBuses_[parent].touched = true;
        }
    }
}

} // namespace rocks::audio
