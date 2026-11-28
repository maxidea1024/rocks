#include "AudioDeviceContext.h"

#include <Core/Diagnostics/Log.h>

// 버스 표·라우팅이다(Docs/Design/AudioSystem 3.45.7·3.45.8절, UE FMixerDevice submix 등록·
// FMixerSource submix send 대응). 키마다 장치에 버스 하나를 두고, 믹서 voice 수명과 자식 버스가
// 참조를 쥔다. 참조가 0이 되면 다음 plan에서 빼고 채택 ACK 뒤 칸을 놓는다.
namespace rocks {

namespace {

inline constexpr LogCategory LogAudioBuses{"AudioBuses"};

bool SameBusParams(const audio::AudioBusParams& a, const audio::AudioBusParams& b) noexcept {
    return a.outputGain == b.outputGain && a.wetGain == b.wetGain && a.dryGain == b.dryGain;
}

bool SameSend(const audio::AudioVoiceSend& a, const audio::AudioVoiceSend& b) noexcept {
    return a.bus.index == b.bus.index && a.bus.generation == b.bus.generation &&
           a.level == b.level && a.stage == b.stage && a.lowPassEnabled == b.lowPassEnabled &&
           a.lowPassCutoffHz == b.lowPassCutoffHz && a.highPassEnabled == b.highPassEnabled &&
           a.highPassCutoffHz == b.highPassCutoffHz;
}

bool SameRoute(const audio::AudioVoiceRoute& a, const audio::AudioVoiceRoute& b) noexcept {
    if (a.sendCount != b.sendCount) {
        return false;
    }

    for (uint32 index = 0; index < a.sendCount; ++index) {
        if (!SameSend(a.sends[index], b.sends[index])) {
            return false;
        }
    }

    return true;
}

audio::AudioSendStage StageOf(MixBusSendStage stage) noexcept {
    return stage == MixBusSendStage::PreAttenuation ? audio::AudioSendStage::PreAttenuation
                                                    : audio::AudioSendStage::PostAttenuation;
}

} // namespace

uint32 AudioDeviceContext::FindBusEntry(uint64 key) const noexcept {
    for (uint32 index = 0; index < busEntries_.Size(); ++index) {
        if (busEntries_[index].key == key) {
            return index;
        }
    }

    return NoBusEntry;
}

audio::AudioBusHandle AudioDeviceContext::BusHandleOf(uint32 entry) const noexcept {
    return entry == MasterBusEntry ? audio::AudioMixer::MasterBus() : busEntries_[entry].handle;
}

bool AudioDeviceContext::IsBusLive(uint32 entry) const noexcept {
    return entry == MasterBusEntry || (busEntries_[entry].inPlan && !busEntries_[entry].retiring);
}

void AudioDeviceContext::RetainBus(uint32 entry) noexcept {
    if (entry != MasterBusEntry) {
        ++busEntries_[entry].refs;
    }
}

void AudioDeviceContext::ReleaseBusRef(uint32 entry) noexcept {
    if (entry == MasterBusEntry) {
        return;
    }

    BusEntry& bus = busEntries_[entry];
    ROCKS_ASSERT(bus.refs > 0);
    --bus.refs;
    if (bus.refs == 0) {
        RetireBus(entry);
    }
}

void AudioDeviceContext::RetireBus(uint32 entry) noexcept {
    // plan에서 빼고 부모 참조를 놓는다(부모도 0이 되면 함께 빠진다). 칸은 채택 ACK 뒤에 놓는다.
    BusEntry& bus = busEntries_[entry];
    if (bus.retiring) {
        return;
    }

    // 게시된 plan에 든 적이 없으면 plan을 다시 낼 필요 없이 바로 놓는다(쓸데없는 게시로 미채택
    // 칸을 쓰지 않게).
    if (!bus.inPlan && mixer_.ReleaseBus(bus.handle)) {
        const uint32 parent = bus.parent;
        bus = BusEntry{};
        ReleaseBusRef(parent);
        return;
    }

    bus.retiring = true;
    busPlanDirty_ = true;
    const uint32 parent = bus.parent;
    bus.parent = MasterBusEntry;
    ReleaseBusRef(parent);
}

bool AudioDeviceContext::BusHasAncestor(uint32 entry, uint32 ancestor) const noexcept {
    uint32 current = entry;
    for (uint32 step = 0; current != MasterBusEntry && step <= busEntries_.Size(); ++step) {
        if (current == ancestor) {
            return true;
        }

        current = busEntries_[current].parent;
    }

    return false;
}

void AudioDeviceContext::LogBusFallback(uint64 key) noexcept {
    // 키마다 처음 한 번만 세고 알린다. 알린 키 표가 차면 그 사실을 한 번 알리고 더 세지 않는다.
    for (const uint64 logged : loggedBusFallbacks_) {
        if (logged == key) {
            return;
        }
    }

    if (loggedBusFallbacks_.Size() >= loggedBusFallbacks_.Capacity()) {
        if (!busFallbackLogFull_) {
            busFallbackLogFull_ = true;
            Log::Warning(LogAudioBuses,
                         "More mixer buses fell back for lack of slots; further keys are not "
                         "reported");
        }

        return;
    }

    (void)loggedBusFallbacks_.TryPushBack(key);
    ++stats_.busCapacityFallbacks;
    Log::Warning(LogAudioBuses,
                 "Mixer bus slots are exhausted ({Slots} slots); bus {Key} routes to its "
                 "nearest registered ancestor and its other sends are dropped",
                 mixer_.Config().maxBuses, key);
}

uint32 AudioDeviceContext::RegisterBus(const MixBusRef& ref, bool& fallback) noexcept {
    // 사슬을 루트 쪽부터 등록한다(3.45.8절). Master 키에서 멈추고, 부모가 없는 루트는 Master에
    // 붙는다. 자식은 부모 참조를 쥔다.
    fallback = false;
    if (!ref) {
        return MasterBusEntry;
    }

    const MixBusChain& chain = *ref.Get();
    uint32 stop = chain.depth;
    for (uint32 index = 0; index < chain.depth; ++index) {
        if (masterBusKey_ != 0 && chain.links[index].key == masterBusKey_) {
            stop = index;
            break;
        }
    }

    uint32 parent = MasterBusEntry;
    for (uint32 step = stop; step > 0; --step) {
        const MixBusLinkValue& link = chain.links[step - 1];
        // 에셋 부모 키다(루트면 0 — Master 키에서 멈췄어도 실제 부모 키를 둔다).
        const uint64 parentKey = step < chain.depth ? chain.links[step].key : 0;
        uint32 entry = FindBusEntry(link.key);
        bool attach = false;
        if (entry == NoBusEntry) {
            entry = FindBusEntry(0);
            auto acquired =
                entry != NoBusEntry
                    ? mixer_.AcquireBus()
                    : Result<audio::AudioBusHandle>::Failure(ErrorCode::CapacityExceeded);
            if (!acquired) {
                // 칸 부족: 이 버스와 남은 자손은 가장 가까운 등록 조상(없으면 Master)으로 간다.
                LogBusFallback(link.key);
                fallback = true;
                return parent;
            }

            BusEntry& created = busEntries_[entry];
            created = BusEntry{};
            created.key = link.key;
            created.parentKey = parentKey;
            created.serial = chain.serial;
            created.value = link;
            created.handle = acquired.Value();
            created.parent = parent;
            attach = true;
            busPlanDirty_ = true;
        } else {
            BusEntry& existing = busEntries_[entry];
            // 부모를 옮길 때: 더 새 스냅숏이 에셋 부모를 바꿨거나, 에셋 부모는 같은데 칸 대응이
            // 바뀌었다(Master 키 교체 — 3.45.8절).
            const bool newer = chain.serial > existing.serial;
            const bool remapped = existing.parentKey == parentKey;
            if (existing.retiring) {
                // 놓기 전에 다시 쓰이면 다음 plan에 되돌린다.
                existing.retiring = false;
                existing.parent = parent;
                existing.parentKey = newer ? parentKey : existing.parentKey;
                attach = true;
                busPlanDirty_ = true;
            } else if (existing.parent != parent && (newer || remapped)) {
                // 순환이 되면 옛 부모를 둔다.
                if (parent != MasterBusEntry && BusHasAncestor(parent, entry)) {
                    ++stats_.busParentCycles;
                    Log::Warning(LogAudioBuses,
                                 "Mixer bus {Key} would form a parent cycle; keeping its "
                                 "previous parent",
                                 link.key);
                } else {
                    RetainBus(parent);
                    const uint32 previous = existing.parent;
                    existing.parent = parent;
                    existing.parentKey = newer ? parentKey : existing.parentKey;
                    ReleaseBusRef(previous);
                    busPlanDirty_ = true;
                }
            }

            if (chain.serial > existing.serial) {
                existing.serial = chain.serial;
                existing.value = link;
            }
        }

        if (attach) {
            RetainBus(parent);
        }

        parent = entry;
    }

    return parent;
}

audio::AudioBusParams
AudioDeviceContext::BusParamsFor(const MixBusLinkValue& value) const noexcept {
    // 에셋 값 × 실행 배수(UE SetSubmixOutputVolume 등), background mute(UE — 실효 마스터 gain 0).
    float volume = 1.0f;
    float wet = 1.0f;
    float dry = 1.0f;
    for (const BusModifier& modifier : busModifiers_) {
        if (modifier.key == value.key) {
            volume = modifier.volume;
            wet = modifier.wet;
            dry = modifier.dry;
            break;
        }
    }

    const bool muted = value.muteWhenBackgrounded && masterGain_ <= audio::NegligibleBusGain;
    audio::AudioBusParams params;
    params.outputGain =
        muted ? 0.0f : math::Clamp(value.outputVolume * volume, 0.0f, audio::MaxBusGain);
    params.wetGain = math::Clamp(value.wetLevel * wet, 0.0f, audio::MaxBusGain);
    params.dryGain = math::Clamp(value.dryLevel * dry, 0.0f, audio::MaxBusGain);
    return params;
}

MixBusLinkValue AudioDeviceContext::MasterBusValue() const noexcept {
    // 엔진 내장 Master는 믹서 기본값(dry 0)이다.
    MixBusLinkValue value;
    if (masterBus_) {
        value = masterBus_.Get()->links[0];
    }

    return value;
}

bool AudioDeviceContext::PublishPlan() noexcept {
    if (!busPlanDirty_) {
        return true;
    }

    busLinks_.Clear();
    for (const BusEntry& entry : busEntries_) {
        if (entry.key == 0 || entry.retiring) {
            continue;
        }

        audio::AudioBusLink link;
        link.bus = entry.handle;
        link.parent = BusHandleOf(entry.parent);
        link.initialParams = BusParamsFor(entry.value);
        (void)busLinks_.TryPushBack(link);
    }

    auto published =
        mixer_.PublishBusPlan(Span<const audio::AudioBusLink>(busLinks_.Data(), busLinks_.Size()));
    if (!published) {
        // 미채택 plan이 둘이면 다음 묶음에 다시 낸다. 그 밖은 표가 어긋난 것이다(계약 위반).
        if (published.Error() == ErrorCode::Busy) {
            ++stats_.busPlanRetries;
        } else {
            Log::Error(LogAudioBuses, "Mixer bus plan was rejected ({Error})",
                       static_cast<uint32>(published.Error()));
        }

        return false;
    }

    ++stats_.busPlanPublishes;
    busPlanDirty_ = false;
    for (BusEntry& entry : busEntries_) {
        entry.inPlan = entry.key != 0 && !entry.retiring;
    }

    return true;
}

void AudioDeviceContext::ApplyBusSettings(const ControlBatch& batch) noexcept {
    if (batch.busSettingsChanged) {
        masterBus_ = batch.masterBus;
        baseDefaultBus_ = batch.baseDefaultBus;
        const uint64 key = masterBus_ ? masterBus_.Get()->Key() : 0;
        if (key != masterBusKey_) {
            // Master 키가 바뀌면 그 키가 칸 0이 된다(3.45.8절): 부모가 그 키인 칸은 바로 Master
            // 밑으로 옮기고, voice를 쥔 재생의 라우팅은 다시 낸다. 옛 Master 키를 부모로 둔 칸은
            // 다음 등록에서 새 보통 버스 밑으로 옮긴다.
            masterBusKey_ = key;
            for (uint32 index = 0; index < busEntries_.Size(); ++index) {
                BusEntry& entry = busEntries_[index];
                if (key == 0 || entry.key == 0 || entry.retiring || entry.parentKey != key ||
                    entry.parent == MasterBusEntry) {
                    continue;
                }

                const uint32 previous = entry.parent;
                entry.parent = MasterBusEntry;
                busPlanDirty_ = true;
                ReleaseBusRef(previous);
            }

            for (Record& record : records_) {
                record.routeDirty = record.routeDirty || record.state == RecordState::Submitted ||
                                    record.state == RecordState::Deferred;
            }
        }
    }

    for (const BusModifierUpdate& update : batch.busModifiers) {
        BusModifier* found = nullptr;
        for (BusModifier& modifier : busModifiers_) {
            if (modifier.key == update.key) {
                found = &modifier;
                break;
            }
        }

        if (found == nullptr) {
            if (busModifiers_.Size() >= busModifiers_.Capacity()) {
                Log::Warning(LogAudioBuses, "Bus modifier table is full; ignoring bus {Key}",
                             update.key);
                continue;
            }

            (void)busModifiers_.TryPushBack(BusModifier{update.key});
            found = &busModifiers_[busModifiers_.Size() - 1];
        }

        found->volume = update.volume;
        found->wet = update.wet;
        found->dry = update.dry;
    }
}

void AudioDeviceContext::WarnRouteFailure(Record& record) noexcept {
    if (record.routeFailureWarned) {
        return;
    }

    record.routeFailureWarned = true;
    ++stats_.droppedBusSends;
    Log::Warning(LogAudioBuses,
                 "Playback route change could not be published (more than {Limit} buses over the "
                 "voice lifetime or a rejected route); keeping the previous route",
                 MaxVoiceBusRefs);
}

AudioDeviceContext::RouteOutcome AudioDeviceContext::BuildRoute(Record& record,
                                                                audio::AudioVoiceRoute& route,
                                                                uint32* entries, bool& pending,
                                                                bool resolveBase) noexcept {
    // UE 쓰기 순서로 합친다(3.45.7절 — 같은 버스면 나중 값이 level·단계·필터를 덮는다): 기본 버스
    // → (AU7h 리버브) → 감쇠 send → 사운드 send(재생 단위 send를 먼저 합침).
    const SoundPlayable& playable = *record.sound.Get();
    pending = false;
    record.distanceSends = false;
    route.sendCount = 0;
    const float distance = record.spatialized && playable.attenuation.Value().spatialize
                               ? record.listenerDistance
                               : 0.0f;
    uint32 baseIndex = NoBusEntry;
    const auto add = [&](const MixBusRef& ref, float level, audio::AudioSendStage stage,
                         const MixBusSendValue* filters, bool base) noexcept {
        // 라우팅이 찼으면 이미 있는 버스가 아닌 send는 칸을 잡지 않고 버린다.
        if (route.sendCount >= audio::MaxVoiceSends) {
            const uint64 key = ref ? ref.Get()->Key() : 0;
            const uint32 known =
                key == 0 || key == masterBusKey_ ? MasterBusEntry : FindBusEntry(key);
            bool present = false;
            for (uint32 index = 0; index < route.sendCount && known != NoBusEntry; ++index) {
                present = present || route.sends[index].bus.index == BusHandleOf(known).index;
            }

            if (!present) {
                if (!record.routeWarned) {
                    record.routeWarned = true;
                    ++stats_.droppedBusSends;
                    Log::Warning(LogAudioBuses,
                                 "Playback routes to more than {Limit} buses; extra sends are "
                                 "dropped",
                                 audio::MaxVoiceSends);
                }

                return;
            }
        }

        bool fallback = false;
        const uint32 entry = RegisterBus(ref, fallback);
        if (fallback && !base) {
            return;
        }

        const audio::AudioBusHandle handle = BusHandleOf(entry);
        uint32 at = route.sendCount;
        for (uint32 index = 0; index < route.sendCount; ++index) {
            if (route.sends[index].bus.index == handle.index) {
                at = index;
                break;
            }
        }

        if (at == route.sendCount) {
            if (route.sendCount >= audio::MaxVoiceSends) {
                if (!record.routeWarned) {
                    record.routeWarned = true;
                    ++stats_.droppedBusSends;
                    Log::Warning(
                        LogAudioBuses,
                        "Playback routes to more than {Limit} buses; extra sends are dropped",
                        audio::MaxVoiceSends);
                }

                return;
            }

            ++route.sendCount;
        }

        audio::AudioVoiceSend& send = route.sends[at];
        send = audio::AudioVoiceSend{};
        send.bus = handle;
        send.level = level;
        send.stage = stage;
        if (filters != nullptr) {
            send.lowPassEnabled = filters->lowPassEnabled;
            send.lowPassCutoffHz = filters->lowPassHz;
            send.highPassEnabled = filters->highPassEnabled;
            send.highPassCutoffHz = filters->highPassHz;
        }

        entries[at] = entry;
        if (base) {
            baseIndex = at;
        }
    };

    const auto addValue = [&](const MixBusSendValue& value, bool postOnly) noexcept {
        record.distanceSends =
            record.distanceSends || value.levelMethod != MixBusSendLevelMethod::Constant;
        const float level = EvaluateMixBusSendLevel(value, distance, audio::MaxVoiceGain);
        add(value.bus, level,
            postOnly ? audio::AudioSendStage::PostAttenuation : StageOf(value.stage), &value,
            false);
    };

    // 기본 버스: 사운드 bus → 자기 분류의 defaultBus → 엔진 BaseDefault → Master. voice를 받을 때
    // 정하고, 그 voice 동안 다시 낼 때는 그 값을 쓴다.
    if (resolveBase) {
        MixBusRef base = playable.bus;
        if (!base && record.category && record.category.Get()->depth != 0) {
            base = record.category.Get()->links[0].defaultBus;
        }

        record.baseBus = base ? base : baseDefaultBus_;
    }

    add(record.baseBus, playable.enableBaseBus ? 1.0f : 0.0f,
        audio::AudioSendStage::PostAttenuation, nullptr, true);
    if (playable.attenuation.HasValue() && playable.attenuation.Value().enableBusSends) {
        const SoundAttenuation& attenuation = playable.attenuation.Value();
        for (uint32 index = 0; index < attenuation.busSendCount; ++index) {
            addValue(attenuation.busSends[index], true);
        }
    }

    if (playable.enableBusSends) {
        bool merged[audio::MaxVoiceSends] = {};
        for (uint32 index = 0; index < playable.busSendCount; ++index) {
            MixBusSendValue value = playable.busSends[index];
            for (uint32 override = 0; override < record.busOverrideCount; ++override) {
                const PlayBusSend& wanted = record.busOverrides[override];
                if (wanted.bus.Get()->Key() == value.bus.Get()->Key()) {
                    // UE SetSubmixSend는 SendLevel만 바꾼다(거리 방법이면 효과가 없다).
                    value.level = wanted.level;
                    merged[override] = true;
                }
            }

            addValue(value, false);
        }

        for (uint32 override = 0; override < record.busOverrideCount; ++override) {
            if (!merged[override]) {
                const PlayBusSend& wanted = record.busOverrides[override];
                add(wanted.bus, math::Clamp(wanted.level, 0.0f, 1.0f),
                    audio::AudioSendStage::PostAttenuation, nullptr, false);
            }
        }
    }

    // 새 버스는 plan을 게시한 뒤에만 라우팅에 쓴다(3.45.4절).
    (void)PublishPlan();
    if (baseIndex != NoBusEntry && !IsBusLive(entries[baseIndex])) {
        return RouteOutcome::BusPending;
    }

    uint32 kept = 0;
    for (uint32 index = 0; index < route.sendCount; ++index) {
        if (!IsBusLive(entries[index])) {
            pending = true;
            continue;
        }

        route.sends[kept] = route.sends[index];
        entries[kept] = entries[index];
        ++kept;
    }

    route.sendCount = kept;
    return RouteOutcome::Ready;
}

bool AudioDeviceContext::AddVoiceBusRefs(uint32 voiceIndex, const audio::AudioVoiceRoute& route,
                                         const uint32* entries) noexcept {
    // voice 수명이 라우팅에 나온 모든 버스를 쥔다(빠진 버스도 voice를 놓을 때까지 — 3.45.8절).
    VoiceBusRefs& refs = voiceBusRefs_[voiceIndex];
    uint32 added = 0;
    for (uint32 index = 0; index < route.sendCount; ++index) {
        const uint32 entry = entries[index];
        bool held = entry == MasterBusEntry;
        for (uint32 known = 0; known < refs.count && !held; ++known) {
            held = refs.entries[known] == entry;
        }

        added += held ? 0u : 1u;
    }

    if (refs.count + added > MaxVoiceBusRefs) {
        return false;
    }

    for (uint32 index = 0; index < route.sendCount; ++index) {
        const uint32 entry = entries[index];
        bool held = entry == MasterBusEntry;
        for (uint32 known = 0; known < refs.count && !held; ++known) {
            held = refs.entries[known] == entry;
        }

        if (!held) {
            RetainBus(entry);
            refs.entries[refs.count++] = entry;
        }
    }

    return true;
}

void AudioDeviceContext::ReleaseVoiceBusRefs(uint32 voiceIndex) noexcept {
    VoiceBusRefs& refs = voiceBusRefs_[voiceIndex];
    for (uint32 index = 0; index < refs.count; ++index) {
        ReleaseBusRef(refs.entries[index]);
    }

    refs.count = 0;
}

void AudioDeviceContext::ApplySetBusSend(const ControlCommand& command) noexcept {
    Record* record = FindRecord(command.handle);
    if (record == nullptr || command.busSendCount == 0) {
        return;
    }

    // UE FActiveSound::SetSubmixSend: 같은 버스면 level만, 아니면 덧붙인다.
    const PlayBusSend& wanted = command.busSends[0];
    for (uint32 index = 0; index < record->busOverrideCount; ++index) {
        if (record->busOverrides[index].bus.Get()->Key() == wanted.bus.Get()->Key()) {
            record->busOverrides[index].level = wanted.level;
            record->routeDirty = true;
            return;
        }
    }

    if (record->busOverrideCount >= audio::MaxVoiceSends) {
        if (!record->routeWarned) {
            record->routeWarned = true;
            ++stats_.droppedBusSends;
            Log::Warning(LogAudioBuses,
                         "Playback routes to more than {Limit} buses; extra sends are dropped",
                         audio::MaxVoiceSends);
        }

        return;
    }

    record->busOverrides[record->busOverrideCount++] = wanted;
    record->routeDirty = true;
}

void AudioDeviceContext::UpdateBuses() noexcept {
    // 참조 없는 버스(등록만 하고 voice가 쓰지 않음)를 빼고, plan을 게시하고, 빠진 칸을 놓고, 값을
    // 게시한다.
    for (uint32 index = 0; index < busEntries_.Size(); ++index) {
        const BusEntry& entry = busEntries_[index];
        if (entry.key != 0 && !entry.retiring && entry.refs == 0) {
            RetireBus(index);
        }
    }

    (void)PublishPlan();
    for (BusEntry& entry : busEntries_) {
        if (entry.key != 0 && entry.retiring && !entry.inPlan && mixer_.ReleaseBus(entry.handle)) {
            entry = BusEntry{};
        }
    }

    uint32 live = 0;
    for (BusEntry& entry : busEntries_) {
        if (entry.key == 0 || entry.retiring) {
            continue;
        }

        live += entry.inPlan ? 1u : 0u;
        const audio::AudioBusParams params = BusParamsFor(entry.value);
        if ((!entry.sentValid || !SameBusParams(params, entry.sent)) &&
            mixer_.PublishBusParams(entry.handle, params)) {
            entry.sent = params;
            entry.sentValid = true;
        }
    }

    stats_.liveBuses = live;
    const audio::AudioBusParams master = BusParamsFor(MasterBusValue());
    if ((!masterBusSentValid_ || !SameBusParams(master, masterBusSent_)) &&
        mixer_.PublishBusParams(audio::AudioMixer::MasterBus(), master)) {
        masterBusSent_ = master;
        masterBusSentValid_ = true;
    }
}

void AudioDeviceContext::PublishRoutes() noexcept {
    // 바뀐 라우팅(SetBusSend·Master 교체·plan 대기)과 거리 기반 send를 다시 낸다.
    for (Record& record : records_) {
        if (record.state != RecordState::Submitted ||
            !(record.routeDirty || (record.distanceSends && record.spatialized))) {
            continue;
        }

        audio::AudioVoiceRoute route;
        uint32 entries[audio::MaxVoiceSends] = {};
        bool pending = false;
        if (BuildRoute(record, route, entries, pending, false) != RouteOutcome::Ready) {
            continue;
        }

        record.routeDirty = pending;
        if (SameRoute(route, record.route)) {
            continue;
        }

        if (!AddVoiceBusRefs(record.voice.index, route, entries)) {
            WarnRouteFailure(record);
            continue;
        }

        if (mixer_.PublishRoute(record.voice, route)) {
            record.route = route;
        } else {
            WarnRouteFailure(record);
        }
    }
}

} // namespace rocks
