#include <Core/Module/ModuleEntry.h>
#include <Material/Material.h>
#include <Material/Material.module.generated.h>

// Material 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Material)

namespace rocks {

Result<void> RegisterMaterialTypes(ModuleRegistration& registration) noexcept {
    return generated::Material::RegisterModuleTypes(registration);
}

} // namespace rocks
