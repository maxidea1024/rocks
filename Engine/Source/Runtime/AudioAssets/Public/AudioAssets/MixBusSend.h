#pragma once

#include <AudioAssets/MixBusData.h>
#include <AudioAssets/MixBusTypes.h>
#include <CurveAssets/FloatCurveSource.h>
#include <Meta/Annotations.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 버스 send 하나다(Docs/Design/AudioSystem 3.45.7절, UE FSoundSubmixSendInfo —
// SoundSubmixSend.h:81–195). 사운드의 busSends와 감쇠 설정의 busSends 원소다. 감쇠 send는 단계를
// 보지 않고 늘 post다(UE FAttenuationSubmixSendSettings에는 단계가 없다).
//
// 참조 struct다(ObjectRef — Hard): StructRefArray의 원소로만 둔다.
namespace rocks {

class MixBus;
class ObjectRegistry;

ROCKS_STRUCT()
struct MixBusSend {
    ROCKS_GENERATED_BODY()

    explicit MixBusSend(Allocator& allocator) noexcept : levelCurve(allocator) {
    }

    // 대상 버스다.
    ROCKS_PROPERTY() ObjectRef<MixBus> bus;
    // level을 정하는 방법이다(UE SendLevelControlMethod, 기본 Constant).
    ROCKS_PROPERTY() MixBusSendLevelMethod levelMethod = MixBusSendLevelMethod::Constant;
    // Constant의 level(≥ 0)과 [0, 1] 자르기 끄기다(UE SendLevel·DisableManualSendClamp).
    ROCKS_PROPERTY() float level = 1.0f;
    ROCKS_PROPERTY() bool disableLevelClamp = false;
    // DistanceLinear의 level 범위(UE MinSendLevel·MaxSendLevel)와 거리 방법의 거리 범위(cm, ≥ 0 —
    // UE MinSendDistance·MaxSendDistance)다.
    ROCKS_PROPERTY() float minLevel = 0.0f;
    ROCKS_PROPERTY() float maxLevel = 1.0f;
    ROCKS_PROPERTY() float minDistance = DefaultMixBusSendMinDistance;
    ROCKS_PROPERTY() float maxDistance = DefaultMixBusSendMaxDistance;
    // DistanceCurve의 곡선(X = 정규화 거리 0–1, Y = level — UE CustomSendLevelCurve)이다.
    ROCKS_PROPERTY() FloatCurveSource levelCurve;
    // send 필터(Hz, [MinMixBusSendFilterHz, MaxMixBusSendFilterHz] — UE bEnableLPFCutoff·
    // LPFCutoff·bEnableHPFCutoff·HPFCutoff)다.
    ROCKS_PROPERTY() bool lowPassEnabled = false;
    ROCKS_PROPERTY() float lowPassHz = MaxMixBusSendFilterHz;
    ROCKS_PROPERTY() bool highPassEnabled = false;
    ROCKS_PROPERTY() float highPassHz = MinMixBusSendFilterHz;
    // 신호 지점이다(UE SendStage, 기본 post).
    ROCKS_PROPERTY() MixBusSendStage stage = MixBusSendStage::PostAttenuation;
};

// send 하나를 재생 값으로 평탄화한다(버스는 MixBus::MakeRef, 곡선은 쓰일 때만 깊이 복사).
// allocator는 스레드 안전해야 한다. 오류: 버스 참조 없음·찾지 못함 InvalidArgument, MakeRef·곡선
// 평탄화 오류, ValidateMixBusSendValue.
ROCKS_AUDIOASSETS_API Result<MixBusSendValue>
MakeMixBusSendValue(const MixBusSend& send, Allocator& allocator, ObjectRegistry& registry,
                    const ObjectAccessContext& access) noexcept;

} // namespace rocks
