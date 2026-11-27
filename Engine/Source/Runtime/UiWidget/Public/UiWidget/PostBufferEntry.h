#pragma once

#include <Meta/Annotations.h>

namespace rocks::ui {

// 같은 창이나 보관 박스의 같은 채널 번호는 동일한 후처리 텍스처를 가리킨다.
ROCKS_STRUCT()
struct PostBufferEntry {
    ROCKS_GENERATED_BODY()
    ROCKS_PROPERTY() int32 channel = 0;
    ROCKS_PROPERTY() float blurStrength = 0;
};

} // namespace rocks::ui
