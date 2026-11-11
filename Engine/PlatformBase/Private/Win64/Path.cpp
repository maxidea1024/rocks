#include "WindowsPlatformPath.h"

namespace rocks::platform {

PathStyle HostPathStyle() noexcept {
    return WindowsPlatformPath::HostStyle();
}

char NativePathSeparator() noexcept {
    return WindowsPlatformPath::NativeSeparator();
}

} // namespace rocks::platform
