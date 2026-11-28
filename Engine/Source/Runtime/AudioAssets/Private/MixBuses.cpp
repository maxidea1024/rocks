#include "AudioAssetsValidation.h"

#include <AudioAssets/AudioObjectKey.h>
#include <AudioAssets/MixBus.generated.h>
#include <AudioAssets/MixBusSend.generated.h>
#include <Audio/Decibels.h>
#include <Core/Math/MathUtility.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Atomics.h>

// 믹서 버스 에셋의 스냅숏 만들기·값 검사·send level 평가다(Docs/Design/AudioSystem 3.45.7절).
namespace rocks {

namespace {

// 다음 생성 번호다(0은 쓰지 않는다).
uint64 mixBusSerial = 0;

// 레벨 dB가 범위 안인지다.
bool IsMixBusLevelDb(float decibels) noexcept {
    return audio_assets_detail::IsFiniteWithin(decibels, MinMixBusLevelDb, MaxMixBusLevelDb);
}

bool IsSendFilterHz(float hz) noexcept {
    return audio_assets_detail::IsFiniteWithin(hz, MinMixBusSendFilterHz, MaxMixBusSendFilterHz);
}

// 선형 레벨 범위다([−96, 0] dB의 선형 값 — 검사 오차를 위해 위·아래를 조금 연다).
constexpr float MixBusLinearSlack = 1.0e-6f;

bool IsMixBusLinear(float value) noexcept {
    return audio_assets_detail::IsFiniteWithin(value, 0.0f, 1.0f + MixBusLinearSlack);
}

// 버스 하나를 사슬 원소로 옮긴다.
Result<void> FillMixBusLink(const MixBus& bus, MixBusLinkValue& link) noexcept {
    if (!IsMixBusLevelDb(bus.outputVolumeDb) || !IsMixBusLevelDb(bus.wetLevelDb) ||
        !IsMixBusLevelDb(bus.dryLevelDb)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    link.key = AudioObjectKey(bus.Handle());
    link.outputVolume = audio::DecibelsToLinear(bus.outputVolumeDb);
    link.wetLevel = audio::DecibelsToLinear(bus.wetLevelDb);
    link.dryLevel = audio::DecibelsToLinear(bus.dryLevelDb);
    link.muteWhenBackgrounded = bus.muteWhenBackgrounded;
    return Result<void>::Success();
}

} // namespace

uint64 NextMixBusSerial() noexcept {
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&mixBusSerial, uint64{1}) + 1;
}

Result<void> ValidateMixBusChain(const MixBusChain& chain) noexcept {
    if (chain.depth > MaxMixBusDepth) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (chain.depth == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < chain.depth; ++index) {
        const MixBusLinkValue& link = chain.links[index];
        if (link.key == 0 || !IsMixBusLinear(link.outputVolume) || !IsMixBusLinear(link.wetLevel) ||
            !IsMixBusLinear(link.dryLevel)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (uint32 earlier = 0; earlier < index; ++earlier) {
            if (chain.links[earlier].key == link.key) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateMixBusSendValue(const MixBusSendValue& send) noexcept {
    const auto fail = []() noexcept { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    if (!send.bus) {
        return fail();
    }

    if (auto valid = ValidateMixBusChain(*send.bus.Get()); !valid) {
        return valid;
    }

    const bool validMethod = static_cast<uint8>(send.levelMethod) <=
                             static_cast<uint8>(MixBusSendLevelMethod::DistanceCurve);
    const bool validStage =
        static_cast<uint8>(send.stage) <= static_cast<uint8>(MixBusSendStage::PreAttenuation);
    if (!validMethod || !validStage || !audio_assets_detail::IsNonNegativeFinite(send.level) ||
        !math::IsFinite(send.minLevel) || !math::IsFinite(send.maxLevel) ||
        !audio_assets_detail::IsNonNegativeFinite(send.minDistance) ||
        !audio_assets_detail::IsNonNegativeFinite(send.maxDistance) ||
        (send.lowPassEnabled && !IsSendFilterHz(send.lowPassHz)) ||
        (send.highPassEnabled && !IsSendFilterHz(send.highPassHz)) ||
        (send.levelMethod == MixBusSendLevelMethod::DistanceCurve && !send.levelCurve)) {
        return fail();
    }

    return Result<void>::Success();
}

float EvaluateMixBusSendLevel(const MixBusSendValue& send, float distance,
                              float maxClampedLevel) noexcept {
    if (send.levelMethod == MixBusSendLevelMethod::Constant) {
        return send.disableLevelClamp ? math::Clamp(send.level, 0.0f, maxClampedLevel)
                                      : math::Clamp(send.level, 0.0f, 1.0f);
    }

    // 정규화 거리(UE — 분모는 1 이상)다.
    const float range = math::Max(send.maxDistance - send.minDistance, MinMixBusSendDistanceRange);
    const float alpha = math::Clamp((distance - send.minDistance) / range, 0.0f, 1.0f);
    float level = 0.0f;
    if (send.levelMethod == MixBusSendLevelMethod::DistanceLinear) {
        level = send.minLevel + (send.maxLevel - send.minLevel) * alpha;
    } else {
        level = send.levelCurve ? send.levelCurve->Evaluate(alpha) : 0.0f;
    }

    return math::IsFinite(level) ? math::Clamp(level, 0.0f, 1.0f) : 0.0f;
}

Result<MixBusRef> MixBus::MakeRef(Allocator& allocator, ObjectRegistry& registry,
                                  const ObjectAccessContext& access) const noexcept {
    using Made = Result<MixBusRef>;
    auto made = SharedPtr<MixBusChain>::TryMake(allocator);
    if (!made) {
        return Made::Failure(made.Error());
    }

    MixBusChain& chain = *made.Value().Get();
    if (auto filled = FillMixBusLink(*this, chain.links[0]); !filled) {
        return Made::Failure(filled.Error());
    }

    chain.depth = 1;
    ObjectHandle next = parent.Handle();
    while (!next.IsNull()) {
        // 순환이면 같은 키를 다시 만난다(깊이 상한 전에 거부한다).
        const uint64 key = AudioObjectKey(next);
        for (uint32 index = 0; index < chain.depth; ++index) {
            if (chain.links[index].key == key) {
                return Made::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (chain.depth >= MaxMixBusDepth) {
            return Made::Failure(ErrorCode::CapacityExceeded);
        }

        auto resolved = registry.Resolve<MixBus>(access, next);
        if (!resolved) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        auto bus = resolved.Value().Get();
        if (!bus) {
            return Made::Failure(ErrorCode::InvalidArgument);
        }

        if (auto filled = FillMixBusLink(*bus.Value(), chain.links[chain.depth]); !filled) {
            return Made::Failure(filled.Error());
        }

        ++chain.depth;
        next = bus.Value()->parent.Handle();
    }

    chain.serial = NextMixBusSerial();
    return Made::Success(made.Value());
}

Result<MixBusSendValue> MakeMixBusSendValue(const MixBusSend& send, Allocator& allocator,
                                            ObjectRegistry& registry,
                                            const ObjectAccessContext& access) noexcept {
    using Made = Result<MixBusSendValue>;
    if (send.bus.Handle().IsNull()) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto resolved = registry.Resolve<MixBus>(access, send.bus.Handle());
    if (!resolved || !resolved.Value().Get()) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto busRef = resolved.Value().Get().Value()->MakeRef(allocator, registry, access);
    if (!busRef) {
        return Made::Failure(busRef.Error());
    }

    MixBusSendValue value;
    value.bus = busRef.Value();
    value.levelMethod = send.levelMethod;
    value.level = send.level;
    value.disableLevelClamp = send.disableLevelClamp;
    value.minLevel = send.minLevel;
    value.maxLevel = send.maxLevel;
    value.minDistance = send.minDistance;
    value.maxDistance = send.maxDistance;
    value.lowPassEnabled = send.lowPassEnabled;
    value.lowPassHz = send.lowPassHz;
    value.highPassEnabled = send.highPassEnabled;
    value.highPassHz = send.highPassHz;
    value.stage = send.stage;
    if (send.levelMethod == MixBusSendLevelMethod::DistanceCurve) {
        if (auto flattened = audio_assets_detail::FlattenCurve(send.levelCurve, allocator, registry,
                                                               access, value.levelCurve);
            !flattened) {
            return Made::Failure(flattened.Error());
        }
    }

    if (auto valid = ValidateMixBusSendValue(value); !valid) {
        return Made::Failure(valid.Error());
    }

    return Made::Success(Move(value));
}

} // namespace rocks
