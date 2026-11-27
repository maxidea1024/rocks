#include <Core/Module/ModuleEntry.h>
#include <CurveAssets/CurveAssets.module.generated.h>
#include <CurveAssets/CurveAssetsModule.h>

// CurveAssets 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, CurveAssets)

namespace rocks {

Result<void> RegisterCurveAssetsTypes(ModuleRegistration& registration) noexcept {
    return generated::CurveAssets::RegisterModuleTypes(registration);
}

} // namespace rocks
