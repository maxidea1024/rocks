#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 텍스처 slot의 텍스처 종류다(Docs/Design/MaterialSystem 12.8절 MS3d2 — 번역 IR 텍스처
// 종류와 같은 값, 실행 시 로드가 읽는 런타임 형).
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialTextureKind : uint8 {
    None = 0,
    Texture2D = 1,
    TextureCube = 2,
    Texture2DArray = 3,
    TextureCubeArray = 4,
    Texture3D = 5,
    External = 6
};

} // namespace rocks::material
