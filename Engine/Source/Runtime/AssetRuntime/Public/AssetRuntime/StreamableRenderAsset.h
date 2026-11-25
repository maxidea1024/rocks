#pragma once

#include <AssetRuntime/Package.h>
#include <Base/NumericLimits.h>

namespace rocks {

// 텍스처와 뒤에 올 메시가 함께 쓰는 LOD·mip 스트리밍 기반이다(UE UStreamableRenderAsset —
// Docs/Design/AssetSystem 12.1절, A5d 12.8절). 그래픽 의존 없는 데이터 기반이라 AssetRuntime에
// 둔다. 저장 설정 셋(NeverStream·GlobalForceMipLevelsToBeResident·NumCinematicMipLevels — UE
// StreamableRenderAsset.h 343·370·374)은 상주 계획이 메타로 읽는다. 실행 중 스트리밍 상태는
// TextureStreaming 모듈의 표에 두고 객체에는 그 표 handle 하나만 둔다(저장하지 않음 — 렌더 lane이
// 바꾸는 값을 게임 lane 객체에 쓰지 않는다, RenderBase 5절).
ROCKS_CLASS(Abstract)
class ROCKS_ASSETRUNTIME_API StreamableRenderAsset : public Asset {
    ROCKS_GENERATED_BODY()

public:
    // 스트리밍 표 handle의 "없음" 값이다.
    static constexpr uint32 NoStreamingHandle = NumericLimits<uint32>::Max();

    // 스트리밍하지 않는다(모든 mip 상주).
    ROCKS_PROPERTY() uint8 neverStream : 1 = 0;
    // 모든 mip을 강제로 상주시킨다(전역 강제 — 저장 설정).
    ROCKS_PROPERTY()
    uint8 globalForceMipLevelsToBeResident : 1 = 0;
    // 시네마틱 mip 수다(계약 — 0이 아니면 UnsupportedSetting, 12.5.1절).
    ROCKS_PROPERTY() int32 numCinematicMipLevels = 0;

    // TextureStreaming 표 handle이다(게임 lane 전용, 저장하지 않음).
    ROCKS_NATIVE() uint32 streamingHandle = NoStreamingHandle;
};

} // namespace rocks
