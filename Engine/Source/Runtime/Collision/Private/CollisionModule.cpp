#include <Collision/Collision.module.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionSettings.h>
#include <Config/ConfigContext.h>
#include <Core/Module/ModuleEntry.h>

namespace rocks {

namespace {

// Collision 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.2절). 시작할 일은 없고, 충돌 profile
// 설정(Docs/Design/CollisionData.ko.md 3절)은 HostSettings가 설정 파일을 읽기 전에 모든 모듈의
// RegisterSettings를 부를 때 등록한다.
class CollisionModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return context.RegisterSettings<CollisionSettings>();
    }
};

} // namespace

Result<void> RegisterCollisionTypes(ModuleRegistration& registration) noexcept {
    return generated::Collision::RegisterModuleTypes(registration);
}

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::CollisionModule, Collision)
