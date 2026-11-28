#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/MixBusData.h>
#include <AudioAssets/MixBusTypes.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRefs.h>

// 믹서 버스 에셋이다(Docs/Design/AudioSystem 3.45.7절, UE USoundSubmix 대응). 부모를 가리키는 한
// 방향 계층이고, 부모가 없으면 Master에 붙는다. 재생과 엔진 API는 MakeRef가 만든 사슬 스냅숏을
// 쓴다. 장치에는 키마다 버스 하나가 있고, 생성 번호가 큰 스냅숏이 값을 바꾼다(3.45.8절).
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다.
//
// 사용 예:
//   auto bus = musicBus.MakeRef(allocator, registry, registry.OwnerAccess());
//   (void)audio->SetBusOutputVolume(bus.Value(), 0.5f);
namespace rocks {

class ObjectRegistry;

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API MixBus final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit MixBus(Allocator& allocator) noexcept {
        (void)allocator;
    }

    // 부모 버스다(UE ParentSubmix). null이면 Master에 붙는다. 순환은 MakeRef가 거부한다.
    ROCKS_PROPERTY() ObjectRef<MixBus> parent;
    // 출력·wet·dry 레벨(dB, [MinMixBusLevelDb, MaxMixBusLevelDb])이다(UE OutputVolume·WetLevel·
    // DryLevelModulation). wet은 효과 체인(AU7g)이 있을 때만 건다.
    ROCKS_PROPERTY() float outputVolumeDb = 0.0f;
    ROCKS_PROPERTY() float wetLevelDb = 0.0f;
    ROCKS_PROPERTY() float dryLevelDb = DefaultMixBusDryLevelDb;
    // 실효 마스터 gain이 0이면 출력을 0으로 한다(UE bMuteWhenBackgrounded — 3.45.8절).
    ROCKS_PROPERTY() bool muteWhenBackgrounded = false;

    // 이 버스부터 루트까지의 사슬 스냅숏이다. allocator는 스레드 안전해야 한다(제어 pipe가 마지막
    // 참조를 놓는다). 오류: 깊이 초과 CapacityExceeded, 순환·범위 밖 값·부모를 찾지 못함
    // InvalidArgument, 할당 실패.
    Result<MixBusRef> MakeRef(Allocator& allocator, ObjectRegistry& registry,
                              const ObjectAccessContext& access) const noexcept;
};

} // namespace rocks
