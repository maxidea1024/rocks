#include <Platform/Path.h>

namespace rocks::test {

platform::PathStyle ExpectedHostPathStyle() noexcept {
    return platform::PathStyle::Posix;
}

char ExpectedHostNativePathSeparator() noexcept {
    return '/';
}

} // namespace rocks::test
