#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 텍스처 slot의 출처다(Docs/Design/MaterialSystem 12.8절 MS3d2 — 참조 텍스처 또는 텍스처
// 파라미터).
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialTextureSource : uint8 {
    Asset = 0,
    Parameter = 1
};

} // namespace rocks::material
