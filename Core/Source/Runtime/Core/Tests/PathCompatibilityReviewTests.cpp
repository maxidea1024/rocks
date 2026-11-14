#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <TestSupport/Test.h>

using namespace rocks;

// UE 5.6은 축약 뒤 "./"를 모두 지워 "a..//"가 "a./", "a.../x"가 "a..x"가 되었다. UE 5.8은 이
// 일괄 제거를 없애고 "/."로 시작하는 구성 요소만 보므로, 축약할 "/."·"/.."가 없으면 이름 안의
// 점을 그대로 둔다. Rocks는 여기에 더해 겹친 구분자만 하나로 줄인다("a..//" → "a../").
TEST(PathCompatibilityKeepsOverlappingDotsLikeUe58) {
    SystemAllocator allocator;
    struct Case {
        StringView input;
        StringView expected;
    };
    const Case cases[] = {
        {"a..//", "a../"},  {"a.../x", "a.../x"},    {"a/b./c", "a/b./c"},
        {"a/...", "a/..."}, {"a/.../b/..", "a/..."},
    };
    const PathStyle styles[] = {PathStyle::Windows, PathStyle::Posix};
    const PathCollapseMode modes[] = {PathCollapseMode::RequireAll, PathCollapseMode::AllPossible};
    for (PathStyle style : styles) {
        for (PathCollapseMode mode : modes) {
            for (const Case& sample : cases) {
                auto value =
                    Path::CollapseRelativeDirectories(allocator, sample.input, style, mode);
                REQUIRE(value);
                CHECK(value.Value().View() == sample.expected);
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathCompatibilityRejectsWindowsDeviceNamespace) {
    SystemAllocator allocator;
    const StringView paths[] = {"\\\\?\\C:\\folder\\file.ext", "//?/C:/folder/file.ext", "//./CON"};
    for (StringView path : paths) {
        auto normalized = Path::NormalizeFilename(allocator, path, PathStyle::Windows);
        auto collapsed = Path::CollapseRelativeDirectories(allocator, path, PathStyle::Windows);
        auto file = Path::FileName(path, PathStyle::Windows);
        auto directory = Path::Directory(path, PathStyle::Windows);
        auto extension = Path::Extension(path, PathStyle::Windows);
        auto split = Path::Split(path, PathStyle::Windows);
        CHECK(!normalized && normalized.Error() == ErrorCode::Unsupported);
        CHECK(!collapsed && collapsed.Error() == ErrorCode::Unsupported);
        CHECK(!file && file.Error() == ErrorCode::Unsupported);
        CHECK(!directory && directory.Error() == ErrorCode::Unsupported);
        CHECK(!extension && extension.Error() == ErrorCode::Unsupported);
        CHECK(!split && split.Error() == ErrorCode::Unsupported);
    }

    // POSIX의 같은 바이트는 장치 구문이 아니다.
    auto posix = Path::NormalizeFilename(allocator, "//?/C:/file", PathStyle::Posix);
    REQUIRE(posix);
    CHECK(posix.Value().View() == StringView("//?/C:/file"));
}

TEST(PathCompatibilitySplitsIncompleteWindowsUncAsText) {
    SystemAllocator allocator;
    const StringView input = "//server";
    auto root = Path::Root(input, PathStyle::Windows);
    auto file = Path::FileName(input, PathStyle::Windows);
    auto directory = Path::Directory(input, PathStyle::Windows);
    auto extension = Path::Extension(input, PathStyle::Windows);
    auto split = Path::Split(input, PathStyle::Windows);
    auto changed = Path::SetExtension(allocator, input, "txt", PathStyle::Windows);

    CHECK(!root && root.Error() == ErrorCode::InvalidArgument);
    REQUIRE(file && directory && extension && split && changed);
    CHECK(file.Value() == StringView("server"));
    CHECK(directory.Value() == StringView("/"));
    CHECK(extension.Value().IsEmpty());
    CHECK(split.Value().directory == StringView("/"));
    CHECK(split.Value().name == StringView("server"));
    CHECK(split.Value().extension.IsEmpty());
    CHECK(changed.Value().View() == StringView("//server.txt"));
}

TEST(PathCompatibilityRejectsMalformedTextBeforeReturningViews) {
    const char invalid[] = {static_cast<char>(0xc0), static_cast<char>(0xaf)};
    const char embeddedNull[] = {'a', '\0', 'b'};
    const StringView inputs[] = {StringView(invalid, sizeof(invalid)),
                                 StringView(embeddedNull, sizeof(embeddedNull))};
    for (StringView input : inputs) {
        auto file = Path::FileName(input, PathStyle::Windows);
        auto directory = Path::Directory(input, PathStyle::Windows);
        auto split = Path::Split(input, PathStyle::Windows);
        CHECK(!file && file.Error() == ErrorCode::InvalidArgument);
        CHECK(!directory && directory.Error() == ErrorCode::InvalidArgument);
        CHECK(!split && split.Error() == ErrorCode::InvalidArgument);
    }
}
