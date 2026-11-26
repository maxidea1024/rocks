#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 메시 머티리얼 슬롯의 UV 채널 밀도다(Docs/Design/MeshAnimationAssets 4.3절·5.5.1절 4, UE
// FMeshUVChannelInfo — MeshUVChannelInfo.h).
namespace rocks {

// 텍스처 스트리밍 UV 밀도 채널 수다(UE MAX_TEXCOORDS = 4 — MeshUVChannelInfo.h, 그 위 UV 채널은
// 스트리밍 계산에 쓰지 않는다).
inline constexpr uint32 MeshUvDensityChannels = 4;

// 슬롯의 UV 채널 밀도다(UE FMeshUVChannelInfo). 텍스처 스트리밍이 표본 크기 입력으로 쓴다. 계산(UE
// UpdateUVChannelData)은 소비자인 텍스처 스트리밍 단계에서 하고, 그때까지 빌드는 값을 바꾸지 않는다
// (5.5.1절 4). 기본값은 UE 생성자처럼 모두 0·거짓이다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshUvDensity {
    ROCKS_GENERATED_BODY()

    // 밀도를 계산했는지다(UE bInitialized).
    ROCKS_PROPERTY() bool initialized = false;
    // 사용자가 밀도를 덮어썼는지다(UE bOverrideDensities — 참이면 계산이 바꾸지 않는다).
    ROCKS_PROPERTY() bool overrideDensities = false;
    // UV 채널별 밀도다(UE LocalUVDensities).
    ROCKS_PROPERTY() float localUvDensities[MeshUvDensityChannels] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

} // namespace rocks
