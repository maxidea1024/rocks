#include <Core/Module/ModuleEntry.h>
#include <CurveAtlas/CurveAtlas.module.generated.h>
#include <CurveAtlas/CurveAtlasModule.h>

// CurveAtlas 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, CurveAtlas)

namespace rocks {

Result<void> RegisterCurveAtlasTypes(ModuleRegistration& registration) noexcept {
    return generated::CurveAtlas::RegisterModuleTypes(registration);
}

} // namespace rocks
