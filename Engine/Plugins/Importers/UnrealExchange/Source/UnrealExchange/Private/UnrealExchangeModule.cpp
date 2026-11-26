#include <Core/Module/ModuleEntry.h>
#include <UnrealExchange/UnrealExchange.module.generated.h>
#include <UnrealExchange/UnrealExchangeTypes.h>

// UnrealExchange 모듈의 진입점과 리플렉션 등록이다(Docs/Design/ModuleLoader 3.2절, UnrealExchange
// 10.6절 3번 — 임포트 옵션 struct). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, UnrealExchange)

namespace rocks {

Result<void> RegisterUnrealExchangeTypes(ModuleRegistration& registration) noexcept {
    return generated::UnrealExchange::RegisterModuleTypes(registration);
}

} // namespace rocks
