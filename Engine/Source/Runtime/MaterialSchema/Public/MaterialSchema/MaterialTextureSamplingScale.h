#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 텍스처 sampling scale이다(Docs/Design/MaterialSystem 12.5절, TextureStreaming T4, UE
// FMaterialTextureInfo). 번역이 텍스처 slot마다 UV 채널과 scale을 계산해 넣는다.
namespace rocks::material {

// UV 채널 없음 표지다(UE INDEX_NONE).
inline constexpr int32 NoUvChannel = -1;

ROCKS_STRUCT()
struct MaterialTextureSamplingScale {
    ROCKS_GENERATED_BODY()

    // 번역 결과의 텍스처 slot 번호(MaterialTexture<k>)다.
    ROCKS_PROPERTY() uint32 textureSlot = 0;
    // 기본값은 UE FMaterialTextureInfo와 같다(UVChannelIndex INDEX_NONE, SamplingScale 0). UE는
    // 텍스처 이름·참조로 잇고 Rocks는 번역 결과 텍스처 slot 번호로 잇는다(12.5절).
    ROCKS_PROPERTY() int32 uvChannel = NoUvChannel;
    ROCKS_PROPERTY() float samplingScale = 0.0f;
};

} // namespace rocks::material
