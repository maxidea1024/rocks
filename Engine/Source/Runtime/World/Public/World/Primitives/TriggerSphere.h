#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Primitives/TriggerBase.h>

namespace rocks {

// 구 Trigger다(UE ATriggerSphere). root는 SphereComponent(반지름 40)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerSphere : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerSphere(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
