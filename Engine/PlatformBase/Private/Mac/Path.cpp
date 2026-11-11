#include "../Apple/ApplePlatformPath.h"

namespace rocks::platform {

PathStyle HostPathStyle() noexcept {
    return ApplePlatformPath::HostStyle();
}

char NativePathSeparator() noexcept {
    return ApplePlatformPath::NativeSeparator();
}

} // namespace rocks::platform
