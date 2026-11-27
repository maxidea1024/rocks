#include <Core/Module/ModuleEntry.h>
#include <RenderData/MaterialEnums.generated.h>
#include <RenderData/RenderData.module.generated.h>
#include <RenderData/RenderDataModule.h>

// RenderData 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, RenderData)

namespace rocks {

Result<void> RegisterRenderDataTypes(ModuleRegistration& registration) noexcept {
    return generated::RenderData::RegisterModuleTypes(registration);
}

} // namespace rocks
