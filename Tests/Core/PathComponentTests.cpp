#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Tests/Support/Test.h>
#include <string.h>

using namespace rocks;

namespace {

bool Same(StringView actual, const char* expected) noexcept {
    const usize bytes = strlen(expected);
    return actual.ByteCount() == bytes && memcmp(actual.Data(), expected, bytes) == 0;
}

// 구성 요소를 '|'로 이어 기대값과 비교한다.
bool ComponentsAre(StringView path, PathStyle style, const char* expected) noexcept {
    auto range = Path::Components(path, style);
    if (!range) {
        return false;
    }
    char joined[256];
    usize length = 0;
    bool first = true;
    for (StringView part : range.Value()) {
        if (!first) {
            joined[length++] = '|';
        }
        first = false;
        if (part.IsEmpty() || length + part.ByteCount() >= sizeof(joined)) {
            return false; // 빈 구성 요소는 나오지 않아야 한다
        }
        memcpy(joined + length, part.Data(), part.ByteCount());
        length += part.ByteCount();
    }
    return Same(StringView(joined, length), expected);
}

} // namespace

TEST(Path_ComponentsSkipEmptyAndKeepDots) {
    CHECK(ComponentsAre("/usr//lib/", PathStyle::Posix, "usr|lib"));
    CHECK(ComponentsAre("a/./../b", PathStyle::Posix, "a|.|..|b"));
    CHECK(ComponentsAre("a\\b/c", PathStyle::Posix, "a\\b|c")); // POSIX에서 '\'는 문자다
    CHECK(ComponentsAre("C:\\a\\\\b/c\\", PathStyle::Windows, "a|b|c"));
    CHECK(ComponentsAre("C:rel\\x", PathStyle::Windows, "rel|x"));
    CHECK(ComponentsAre("\\\\server\\share\\dir\\f.txt", PathStyle::Windows, "dir|f.txt"));
    CHECK(ComponentsAre("", PathStyle::Posix, ""));
    CHECK(ComponentsAre("/", PathStyle::Posix, ""));
    CHECK(ComponentsAre("///", PathStyle::Posix, ""));
    CHECK(ComponentsAre("C:\\", PathStyle::Windows, ""));
    CHECK(
        ComponentsAre("\xed\x95\x9c/\xea\xb8\x80", PathStyle::Posix, "\xed\x95\x9c|\xea\xb8\x80"));
    // 오류는 Root와 같다.
    CHECK(Path::Components(StringView("a\0b", 3), PathStyle::Posix).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Path::Components(StringView("\xff", 1), PathStyle::Posix).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Path::Components("\\\\?\\C:\\x", PathStyle::Windows).Error() == ErrorCode::Unsupported);
    // NT object namespace도 거부한다(역슬래시·슬래시 모두).
    CHECK(Path::Components("\\??\\C:\\x", PathStyle::Windows).Error() == ErrorCode::Unsupported);
    CHECK(Path::Components("/\?\?/C:/x", PathStyle::Windows).Error() == ErrorCode::Unsupported);
    CHECK(Path::Components("\\\\server", PathStyle::Windows).Error() ==
          ErrorCode::InvalidArgument); // 불완전한 UNC
    CHECK(ComponentsAre("//a", PathStyle::Posix, "a"));
    CHECK(ComponentsAre("\\\\server\\share", PathStyle::Windows, ""));
    CHECK(Same(Path::PathLeaf("\\\\server\\share", PathStyle::Windows).Value(), ""));
    // 반복자 비교: 기본 생성끼리는 같고, 서로 다른 경로의 반복자는 다르다.
    CHECK(PathComponentRange::Iterator() == PathComponentRange::Iterator());
    auto first = Path::Components("x/y", PathStyle::Posix);
    auto second = Path::Components("p/q", PathStyle::Posix);
    REQUIRE(first && second);
    CHECK(!(first.Value().begin() == second.Value().begin()));
    // 반복자는 forward 반복 규약을 따른다.
    auto range = Path::Components("x/y", PathStyle::Posix);
    REQUIRE(range);
    auto it = range.Value().begin();
    auto copy = it++;
    CHECK(Same(*copy, "x") && Same(*it, "y") && ++it == range.Value().end());
}

TEST(Path_RootPartAndPathLeaf) {
    CHECK(Same(Path::RootPart("/usr/lib", PathStyle::Posix).Value(), "/"));
    CHECK(Same(Path::RootPart("usr/lib", PathStyle::Posix).Value(), ""));
    CHECK(Same(Path::RootPart("C:\\a", PathStyle::Windows).Value(), "C:\\"));
    CHECK(Same(Path::RootPart("C:a", PathStyle::Windows).Value(), "C:"));
    CHECK(Same(Path::RootPart("\\a", PathStyle::Windows).Value(), "\\"));
    // UNC 루트는 공유 이름 뒤 구분자가 있으면 포함한다(다른 절대 루트와 같이 바로 이어 붙일 수
    // 있다).
    CHECK(Same(Path::RootPart("\\\\server\\share\\dir", PathStyle::Windows).Value(),
               "\\\\server\\share\\"));
    CHECK(
        Same(Path::RootPart("\\\\server\\share", PathStyle::Windows).Value(), "\\\\server\\share"));
    CHECK(Path::RootPart("\\\\server", PathStyle::Windows).Error() == ErrorCode::InvalidArgument);

    CHECK(Same(Path::PathLeaf("a/b/", PathStyle::Posix).Value(), "b"));
    CHECK(Same(Path::PathLeaf("a/b//", PathStyle::Posix).Value(), "b"));
    CHECK(Same(Path::PathLeaf("a/b.txt", PathStyle::Posix).Value(), "b.txt"));
    CHECK(Same(Path::PathLeaf("name", PathStyle::Posix).Value(), "name"));
    CHECK(Same(Path::PathLeaf("/", PathStyle::Posix).Value(), ""));
    CHECK(Same(Path::PathLeaf("", PathStyle::Posix).Value(), ""));
    CHECK(Same(Path::PathLeaf("C:\\dir\\", PathStyle::Windows).Value(), "dir"));
    CHECK(Same(Path::PathLeaf("C:\\", PathStyle::Windows).Value(), ""));
    CHECK(Same(Path::PathLeaf("C:x", PathStyle::Windows).Value(), "x"));
    CHECK(Same(Path::PathLeaf("a\\b\\", PathStyle::Posix).Value(), "a\\b\\")); // POSIX
    // FileName과의 차이: 끝 구분자가 있으면 FileName은 빈 값이다.
    CHECK(Path::FileName("a/b/", PathStyle::Posix).Value().IsEmpty());
}

TEST(Path_MakePlatformFilename) {
    SystemAllocator allocator;
    auto windows = Path::MakePlatformFilename(allocator, "C:/a/b\\c/", PathStyle::Windows);
    REQUIRE(windows);
    CHECK(Same(windows.Value().View(), "C:\\a\\b\\c\\"));
    auto unc = Path::MakePlatformFilename(allocator, "//server/share/x", PathStyle::Windows);
    REQUIRE(unc);
    CHECK(Same(unc.Value().View(), "\\\\server\\share\\x"));
    auto posix = Path::MakePlatformFilename(allocator, "a/b\\c", PathStyle::Posix);
    REQUIRE(posix);
    CHECK(Same(posix.Value().View(), "a/b\\c"));
    // 변환 결과가 device·NT namespace가 되는 입력은 처음부터 거부한다.
    CHECK(Path::MakePlatformFilename(allocator, "//?/C:/x", PathStyle::Windows).Error() ==
          ErrorCode::Unsupported);
    CHECK(Path::MakePlatformFilename(allocator, "/\?\?/C:/Windows", PathStyle::Windows).Error() ==
          ErrorCode::Unsupported);
    CHECK(Path::MakePlatformFilename(allocator, StringView("a\0", 2), PathStyle::Posix).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 3);
}
