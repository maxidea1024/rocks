#include <Core/Module/ModuleEntry.h>
#include <MeshSource/MeshSource.h>
#include <MeshSource/MeshSource.module.generated.h>

// MeshSource 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, MeshSource)

namespace rocks {

Result<void> RegisterMeshSourceTypes(ModuleRegistration& registration) noexcept {
    return generated::MeshSource::RegisterModuleTypes(registration);
}

} // namespace rocks
