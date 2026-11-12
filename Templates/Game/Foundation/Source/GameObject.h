#pragma once

#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace sample {

ROCKS_CLASS()
class GameObject final : public rocks::Object {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY(ReadOnly, Category = "Game", DisplayName = "체력")
    rocks::int32 health = 100;
};

} // namespace sample
