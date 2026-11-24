#include <Curves/CurvesModule.h>

#include <Core/Module/ModuleEntry.h>
#include <Curves/Curves.module.generated.h>

namespace rocks {

Result<void> RegisterCurvesTypes(ModuleRegistration& registration) noexcept {
    return generated::Curves::RegisterModuleTypes(registration);
}

} // namespace rocks

// Curves 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 타입 등록은 ML2에서 진입 기술로
// 옮긴다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Curves)
