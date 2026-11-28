#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 머티리얼 품질 수준이다(Docs/Design/MaterialSystem 8.5·6.9절 — UE EMaterialQualityLevel,
// RE/Public/SceneTypes.h:129–139 순서). 번역은 수준 하나로 Quality Switch를 푼다. None은 UE
// Num(번역 수준 없음 — Default 입력)이다.
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialQualityLevel : uint8 {
    Low = 0,
    High = 1,
    Medium = 2,
    Epic = 3,
    None = 4
};

} // namespace rocks::material
