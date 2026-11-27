#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Virtual Texture 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// RuntimeVirtualTextureAddressMode 값이다(UE ERuntimeVirtualTextureTextureAddressMode —
// RE/Public/Materials/MaterialExpressionRuntimeVirtualTextureSample.h).
ROCKS_ENUM()
enum class RuntimeVirtualTextureAddressMode : uint8 {
    Clamp = 0,
    Wrap = 1
};

// RuntimeVirtualTextureMaterialType 값이다(UE ERuntimeVirtualTextureMaterialType —
// RE/Public/VT/RuntimeVirtualTextureEnum.h).
ROCKS_ENUM()
enum class RuntimeVirtualTextureMaterialType : uint8 {
    BaseColor = 0,
    Mask4 = 1,
    BaseColorNormalRoughness = 2,
    BaseColorNormalSpecular = 3,
    BaseColorNormalSpecularYCoCg = 4,
    BaseColorNormalSpecularMaskYCoCg = 5,
    WorldHeight = 6,
    Displacement = 7
};

// RuntimeVirtualTextureMipValueMode 값이다(UE ERuntimeVirtualTextureMipValueMode —
// RE/Public/Materials/MaterialExpressionRuntimeVirtualTextureSample.h).
ROCKS_ENUM()
enum class RuntimeVirtualTextureMipValueMode : uint8 {
    None = 0,
    MipLevel = 1,
    MipBias = 2,
    DerivativeUV = 4,
    DerivativeWorld = 5
};

} // namespace rocks::material
