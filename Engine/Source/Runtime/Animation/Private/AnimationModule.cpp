#include <Animation/Animation.module.generated.h>
#include <Animation/Skeleton.h>
#include <Core/Module/ModuleEntry.h>

// Animation 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Animation)

namespace rocks {

Result<void> RegisterAnimationTypes(ModuleRegistration& registration) noexcept {
    return generated::Animation::RegisterModuleTypes(registration);
}

} // namespace rocks
