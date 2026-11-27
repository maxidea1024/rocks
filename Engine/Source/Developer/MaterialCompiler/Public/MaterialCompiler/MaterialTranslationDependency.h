#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 번역이 읽은 다른 에셋 하나다(Docs/Design/MaterialSystem 6.12절 MS2e2 — 쿠커가 의존 키를 붙인다,
// 12.2절).
namespace rocks::material {

// 의존 역할이다.
enum class MaterialDependencyRole : uint8 {
    Function = 0,
    Material = 1
};

struct MaterialTranslationDependency {
    MaterialDependencyRole role = MaterialDependencyRole::Function;
    Guid asset;
};

} // namespace rocks::material
