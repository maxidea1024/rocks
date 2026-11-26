#include <Core/Module/ModuleEntry.h>
#include <WorldIntegration/WorldIntegration.module.generated.h>
#include <WorldIntegration/WorldIntegrationTypes.h>

// WorldIntegration 모듈 등록이다. 시작할 일이 없어 기본 수명 객체를 쓴다(렌더 연결은 RS 단계 —
// Docs/Design/EngineRuntime 30.6절).
namespace rocks {

Result<void> RegisterWorldIntegrationTypes(ModuleRegistration& registration) noexcept {
    return generated::WorldIntegration::RegisterModuleTypes(registration);
}

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, WorldIntegration)
