#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Primitives/TriggerBase.h>

namespace rocks {

// 상자 Trigger다(UE ATriggerBox). root는 BoxComponent(extent 40)다.
ROCKS_CLASS()
class ROCKS_WORLD_API TriggerBox : public TriggerBase {
    ROCKS_GENERATED_BODY()

public:
    explicit TriggerBox(Allocator& allocator) noexcept : TriggerBase(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
