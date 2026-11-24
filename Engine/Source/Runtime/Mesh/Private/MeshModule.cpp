#include <Core/Module/ModuleEntry.h>
#include <Mesh/Mesh.module.generated.h>
#include <Mesh/MeshBuildSettings.h>

// Mesh 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Mesh)

namespace rocks {

Result<void> RegisterMeshTypes(ModuleRegistration& registration) noexcept {
    return generated::Mesh::RegisterModuleTypes(registration);
}

} // namespace rocks
