#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/ReflectionCapture.generated.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/Environment/SphereReflectionCapture.generated.h>
#include <World/Environment/SphereReflectionCapture.h>
#include <World/Environment/SphereReflectionCaptureComponent.generated.h>
#include <World/Environment/SphereReflectionCaptureComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/ActorRootSetup.h>

// 환경 Actor SphereReflectionCapture다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> SphereReflectionCapture::OnFrameworkPreAwake() noexcept {
    // root는 SphereReflectionCaptureComponent다(UE ReflectionCaptureComponent.cpp:282).
    return actor_root_setup::PrepareRoot<SphereReflectionCaptureComponent>(
        *this, [](SphereReflectionCaptureComponent&) noexcept { return Result<void>::Success(); });
}

} // namespace rocks
