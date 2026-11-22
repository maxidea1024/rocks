#pragma once

#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace foundationtemplate {

ROCKS_CLASS()
class GameObject final : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY(ReadOnly, Category = "Game", DisplayName = "Health")
    rocks::int32 health = 100;
};

} // namespace foundationtemplate
