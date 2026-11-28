#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 텍스처 slot의 샘플러 출처다(Docs/Design/MaterialSystem 7.2절 9항, 12.8절 MS3d2 —
// 샘플하지 않는 slot None, 자기 샘플러 Own, 공유 Wrap·Clamp 샘플러).
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialSamplerSource : uint8 {
    None = 0,
    Own = 1,
    SharedWrap = 2,
    SharedClamp = 3
};

} // namespace rocks::material
