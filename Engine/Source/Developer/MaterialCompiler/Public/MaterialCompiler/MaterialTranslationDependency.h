#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 번역이 읽은 다른 에셋 하나다(Docs/Design/MaterialSystem 6.12절 MS2e2 — 쿠커가 의존 키를 붙인다,
// 12.2절).
namespace rocks::material {

// 의존 역할이다.
enum class MaterialDependencyRole : uint8 {
    Function = 0,
    Material = 1,
    // ColorRamp의 색 커브다(6.13절 MS2f4).
    ColorCurve = 2,
    // CurveAtlasRowParameter의 색 커브 atlas다(6.10절 MS2c5 — 커브 목록이 행 번호를 정한다).
    CurveAtlas = 3
};

struct MaterialTranslationDependency {
    MaterialDependencyRole role = MaterialDependencyRole::Function;
    Guid asset;
};

} // namespace rocks::material
