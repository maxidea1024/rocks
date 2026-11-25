#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Primitives/TriggerBase.h>

namespace rocks {

// 캡슐 Trigger다(UE ATriggerCapsule). root는 CapsuleComponent(반지름 40·반 높이 80)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerCapsule : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerCapsule(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
