#include <Curves/CurvesModule.h>

#include <Curves/Curves.module.generated.h>

namespace rocks {

Result<void> RegisterCurvesTypes(ModuleRegistration& registration) noexcept {
    return generated::Curves::RegisterModuleTypes(registration);
}

} // namespace rocks
