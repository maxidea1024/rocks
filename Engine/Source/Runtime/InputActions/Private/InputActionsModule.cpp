#include <Core/Module/ModuleEntry.h>
#include <InputActions/InputAction.generated.h>
#include <InputActions/InputActionData.generated.h>
#include <InputActions/InputActionSettings.generated.h>
#include <InputActions/InputActions.module.generated.h>
#include <InputActions/InputActionsModule.h>
#include <InputActions/InputBinding.generated.h>
#include <InputActions/InputContext.generated.h>
#include <InputActions/InputContextData.generated.h>
#include <InputActions/InputDataEnums.generated.h>
#include <InputActions/InputModifier.generated.h>
#include <InputActions/InputProfileBindings.generated.h>
#include <InputActions/InputTrigger.generated.h>
#include <InputActions/InputValue.generated.h>
#include <InputActions/PlayerMappingSettings.generated.h>

// InputActions 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, InputActions)

namespace rocks::input {

Result<void> RegisterInputActionsTypes(ModuleRegistration& registration) noexcept {
    return generated::InputActions::RegisterModuleTypes(registration);
}

} // namespace rocks::input
