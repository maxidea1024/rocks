#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 파라미터 종류다(Docs/Design/MaterialSystem 12.8절 MS3d1 — UE 5.8 EMaterialParameterType,
// RE/Public/Materials/MaterialParameters.h:186–207). UE 순서를 따르고(NumRuntime 앞이 실행 중
// 바꿀 수 있는 종류), UE의 NumRuntime 밖 StaticComponentMask 뒤에 Rocks 고유 종류를 둔다.
//
// 사용 예:
//   if (metadata.type == MaterialParameterType::Texture) { UseDefault(metadata.texture); }
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialParameterType : uint8 {
    Scalar = 0,
    Vector = 1,
    DoubleVector = 2,
    Texture = 3,
    TextureCollection = 4,
    Font = 5,
    RuntimeVirtualTexture = 6,
    SparseVolumeTexture = 7,
    StaticSwitch = 8,
    ParameterCollection = 9,
    // UE NumRuntime 뒤(static 변형만 바꾼다)다.
    StaticComponentMask = 10,
    // Rocks 고유다(static enum 선택 — 9.3절 MAT2, 동적 bool — MAT2).
    StaticEnum = 11,
    DynamicBool = 12
};

} // namespace rocks::material
