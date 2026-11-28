#pragma once

#include <AudioAssets/MixBusTypes.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>
#include <Curves/TangentCurve.h>

// 믹서 버스 스냅숏이다(Docs/Design/AudioSystem 3.45.7절). 게임 스레드의 MixBus::MakeRef가 만들고
// 재생 스냅숏·엔진 API가 공유 참조로 싣는다. 제어 쪽이 Registry 없이 읽는다.
namespace rocks {

// 버스 하나의 값이다(선형 — 10^(dB/20), UE ConvertToLinear).
struct MixBusLinkValue {
    // 버스 객체의 AudioObjectKey다(0이 아니다).
    uint64 key = 0;
    float outputVolume = 1.0f;
    float wetLevel = 1.0f;
    float dryLevel = 0.0f;
    bool muteWhenBackgrounded = false;
};

// links[0]이 그 버스, links[depth − 1]이 루트(부모 없음 — Master에 붙는다)다. serial은 프로세스
// 안에서 단조 증가하는 생성 번호다(같은 키의 값이 둘이면 큰 쪽이 새 값이다 — 3.45.8절).
struct MixBusChain {
    uint64 serial = 0;
    uint32 depth = 0;
    MixBusLinkValue links[MaxMixBusDepth];

    uint64 Key() const noexcept {
        return depth != 0 ? links[0].key : 0;
    }
};

using MixBusRef = SharedPtr<const MixBusChain>;

// send 하나의 값이다(UE FSoundSubmixSendInfo — 3.45.7절). 곡선은 levelMethod가 DistanceCurve일
// 때만 있다.
struct MixBusSendValue {
    MixBusRef bus;
    MixBusSendLevelMethod levelMethod = MixBusSendLevelMethod::Constant;
    float level = 1.0f;
    bool disableLevelClamp = false;
    float minLevel = 0.0f;
    float maxLevel = 1.0f;
    float minDistance = DefaultMixBusSendMinDistance;
    float maxDistance = DefaultMixBusSendMaxDistance;
    SharedPtr<const TangentCurve> levelCurve;
    bool lowPassEnabled = false;
    float lowPassHz = MaxMixBusSendFilterHz;
    bool highPassEnabled = false;
    float highPassHz = MinMixBusSendFilterHz;
    MixBusSendStage stage = MixBusSendStage::PostAttenuation;
};

// 다음 버스 스냅숏 생성 번호다(스레드 안전 — 1부터).
ROCKS_AUDIOASSETS_API uint64 NextMixBusSerial() noexcept;

// 값 검사다: 깊이 0, 키 0·중복(순환), 범위 밖·비유한 값(InvalidArgument — 깊이 초과는
// CapacityExceeded).
ROCKS_AUDIOASSETS_API Result<void> ValidateMixBusChain(const MixBusChain& chain) noexcept;

// send 값 검사다: 버스 없음·사슬 오류, 범위 밖·비유한 값, 거리 방법인데 거리가 음수, 곡선 방법인데
// 곡선 없음(InvalidArgument).
ROCKS_AUDIOASSETS_API Result<void> ValidateMixBusSendValue(const MixBusSendValue& send) noexcept;

// 리스너 거리 distance(cm)에서의 send level이다(UE FMixerSource::UpdateSubmixSendLevels —
// AudioMixerSource.cpp:1554–1597). Constant는 disableLevelClamp가 아니면 [0, 1], 거리 방법은
// α = clamp((d − minDistance) / max(maxDistance − minDistance, 1), 0, 1)로 lerp·곡선 값을 [0, 1]로
// 자른다. disableLevelClamp인 Constant는 [0, maxClampedLevel]로 자른다(믹서 범위).
ROCKS_AUDIOASSETS_API float EvaluateMixBusSendLevel(const MixBusSendValue& send, float distance,
                                                    float maxClampedLevel) noexcept;

} // namespace rocks
