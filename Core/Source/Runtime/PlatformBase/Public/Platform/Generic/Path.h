#pragma once

#include <Platform/Path.h>

namespace rocks::platform {

// 플랫폼 기본 구현이다. 새 호스트는 문법을 명시하기 전까지 미지원으로 취급한다.
// 구분자 '/'가 맞는 호스트는 이 기본 구현을 재사용한다.
struct GenericPlatformPath {
    static constexpr PathStyle HostStyle() noexcept {
        return PathStyle::Unsupported;
    }

    static constexpr char NativeSeparator() noexcept {
        return '/';
    }
};

} // namespace rocks::platform
