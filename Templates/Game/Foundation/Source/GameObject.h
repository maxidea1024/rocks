#pragma once

#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace sample {

ROCKS_CLASS("da100000000000000000000000000001", 1)
class GameObject final : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY("da100000000000000000000000000002", ReadOnly, Category = "Game",
                   DisplayName = "체력")
    rocks::int32 health = 100;
};

} // namespace sample
