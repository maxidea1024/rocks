#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/PlaneReflectionCapture.generated.h>
#include <World/Environment/PlaneReflectionCapture.h>
#include <World/Environment/PlaneReflectionCaptureComponent.generated.h>
#include <World/Environment/PlaneReflectionCaptureComponent.h>
#include <World/Environment/ReflectionCapture.generated.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ActorRootSetup.h"

// 환경 Actor PlaneReflectionCapture다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> PlaneReflectionCapture::OnFrameworkPreAwake() noexcept {
    // root 상대 scale (1, 1000, 1000)이다(UE ReflectionCaptureComponent.cpp:387).
    return actor_root_setup::PrepareRoot<PlaneReflectionCaptureComponent>(
        *this, [](PlaneReflectionCaptureComponent& capture) noexcept {
            return capture.SetRelativeScale(Vec3d(1.0, DefaultScaleYZ, DefaultScaleYZ));
        });
}

} // namespace rocks
