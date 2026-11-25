#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/BoxReflectionCapture.generated.h>
#include <World/Environment/BoxReflectionCapture.h>
#include <World/Environment/BoxReflectionCaptureComponent.generated.h>
#include <World/Environment/BoxReflectionCaptureComponent.h>
#include <World/Environment/ReflectionCapture.generated.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ActorRootSetup.h"

// 환경 Actor BoxReflectionCapture다(Docs/Design/EngineRuntime 27.1절, D5).
namespace rocks {

Result<void> BoxReflectionCapture::OnFrameworkPreAwake() noexcept {
    // root 상대 scale이 상자 크기다(UE ReflectionCaptureComponent.cpp:335).
    return actor_root_setup::PrepareRoot<BoxReflectionCaptureComponent>(
        *this, [](BoxReflectionCaptureComponent& capture) noexcept {
            return capture.SetRelativeScale(Vec3d(DefaultScaleXY, DefaultScaleXY, DefaultScaleZ));
        });
}

} // namespace rocks
