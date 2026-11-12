#pragma once

#include <Platform/Generic/Path.h>

namespace rocks::platform {

// Apple은 POSIX 경로 문법을 선택하고 공통 '/' 구분자를 그대로 사용한다.
struct ApplePlatformPath : GenericPlatformPath {
    static constexpr PathStyle HostStyle() noexcept {
        return PathStyle::Posix;
    }
};

} // namespace rocks::platform
