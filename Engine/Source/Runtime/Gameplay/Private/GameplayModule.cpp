#include <Core/Module/ModuleEntry.h>
#include <Gameplay/Gameplay.module.generated.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/LocalPlayer.generated.h>

// Gameplay 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다 — 리플렉션
// 타입은 엔진의 모듈 타입 등록기가 등록한다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, Gameplay)

namespace rocks {

const char* NetModeName(NetMode mode) noexcept {
    switch (mode) {
    case NetMode::Standalone:
        return "Standalone";
    case NetMode::DedicatedServer:
        return "DedicatedServer";
    case NetMode::ListenServer:
        return "ListenServer";
    case NetMode::Client:
        return "Client";
    }

    return "Unknown";
}

Result<void> RegisterGameplayTypes(ModuleRegistration& registration) noexcept {
    return generated::Gameplay::RegisterModuleTypes(registration);
}

LocalPlayer::LocalPlayer(Allocator& allocator) noexcept : subsystems_(allocator) {
}

LocalPlayer::~LocalPlayer() noexcept = default;

} // namespace rocks
