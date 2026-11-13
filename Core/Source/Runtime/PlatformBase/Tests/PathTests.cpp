#include <Platform/Generic/Path.h>
#include <Platform/Path.h>
#include <PlatformBase/Private/Apple/ApplePlatformPath.h>
#include <PlatformBase/Private/Win64/WindowsPlatformPath.h>
#include <TestSupport/Test.h>

namespace rocks::test {
platform::PathStyle ExpectedHostPathStyle() noexcept;
char ExpectedHostNativePathSeparator() noexcept;
} // namespace rocks::test

TEST(PlatformPathSyntaxMatchesHost) {
    // 프로세스 전역 상태와 무관하게 여러 번 조회해도 같은 문법을 사용해야 한다.
    const auto expected = rocks::test::ExpectedHostPathStyle();
    CHECK(rocks::platform::HostPathStyle() == expected);
    CHECK(rocks::platform::HostPathStyle() == expected);
    CHECK(rocks::platform::NativePathSeparator() == rocks::test::ExpectedHostNativePathSeparator());
}

TEST(PlatformPathGenericAndOverridesAreExplicit) {
    using namespace rocks::platform;
    CHECK(GenericPlatformPath::HostStyle() == PathStyle::Unsupported);
    CHECK(GenericPlatformPath::NativeSeparator() == '/');
    CHECK(ApplePlatformPath::HostStyle() == PathStyle::Posix);
    CHECK(ApplePlatformPath::NativeSeparator() == GenericPlatformPath::NativeSeparator());
    CHECK(WindowsPlatformPath::HostStyle() == PathStyle::Windows);
    CHECK(WindowsPlatformPath::NativeSeparator() == '\\');
}
