#pragma once

#include <Base/Types.h>

// 머티리얼 노드가 들어갈 수 있는 그래프 종류다(Docs/Design/MaterialSystem 5.2절).
namespace rocks::material {

// 노드가 들어갈 수 있는 그래프 종류다(UE UMaterialExpression::IsAllowedIn 대응).
enum class MaterialGraphKind : uint8 {
    Material = 0,
    Function = 1
};

using MaterialGraphKindMask = uint8;

constexpr MaterialGraphKindMask ToMask(MaterialGraphKind kind) noexcept {
    return static_cast<MaterialGraphKindMask>(1u << static_cast<uint32>(kind));
}

inline constexpr MaterialGraphKindMask AllMaterialGraphKinds =
    ToMask(MaterialGraphKind::Material) | ToMask(MaterialGraphKind::Function);

} // namespace rocks::material
