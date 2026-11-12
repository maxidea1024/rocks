#pragma once

#include <Platform/Generic/Path.h>

namespace rocks::platform {

// Windows는 기본 문법과 원시 구분자를 각각 명시한다.
struct WindowsPlatformPath : GenericPlatformPath {
    static constexpr PathStyle HostStyle() noexcept {
        return PathStyle::Windows;
    }

    static constexpr char NativeSeparator() noexcept {
        return '\\';
    }
};

} // namespace rocks::platform
