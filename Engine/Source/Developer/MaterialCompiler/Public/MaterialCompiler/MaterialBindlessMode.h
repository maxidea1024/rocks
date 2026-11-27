#pragma once

#include <Base/Types.h>

// 형식의 bindless 설정이다(Docs/Design/MaterialSystem 6.9절 — UE ERHIBindlessConfiguration,
// Bindless Switch가 읽는다 — RE/Private/Materials/MaterialExpressions.cpp:9510–9535).
namespace rocks::material {

enum class MaterialBindlessMode : uint8 {
    Disabled = 0,
    RayTracing = 1,
    Minimal = 2,
    All = 3
};

} // namespace rocks::material
