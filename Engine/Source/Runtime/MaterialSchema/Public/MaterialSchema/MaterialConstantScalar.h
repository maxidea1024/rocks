#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 상수 식 명령 값의 스칼라 형이다(Docs/Design/MaterialSystem 9.4절, 12.8절 MS3d2 — 실행은 float32,
// LWC 파라미터만 double, Texture는 TextureSize가 읽는 slot 값이다).
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialConstantScalar : uint8 {
    Float = 0,
    Double = 1,
    Texture = 2
};

} // namespace rocks::material
