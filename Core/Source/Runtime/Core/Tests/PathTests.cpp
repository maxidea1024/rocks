#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct NormalizeCase {
    const char* input;
    const char* expected;
    PathStyle style;
};

void CheckNormalized(Allocator& allocator, const NormalizeCase& sample) noexcept {
    auto first = Path::Normalize(allocator, sample.input, sample.style);
    REQUIRE(first);
    CHECK(first.Value().View() == StringView(sample.expected));
    auto second = Path::Normalize(allocator, first.Value().View(), sample.style);
    REQUIRE(second);
    CHECK(second.Value().View() == first.Value().View());
}

void CheckJoin(Allocator& allocator, StringView base, StringView relative, PathStyle style,
               StringView expected) noexcept {
    auto joined = Path::Join(allocator, base, relative, style);
    REQUIRE(joined);
    CHECK(joined.Value().View() == expected);
}

void CheckRelativeRoundTrip(Allocator& allocator, StringView base, StringView path, PathStyle style,
                            StringView expected) noexcept {
    auto relative = Path::MakeRelative(allocator, base, path, style);
    REQUIRE(relative);
    CHECK(relative.Value().View() == expected);
    auto reconstructed = Path::MakeAbsolute(allocator, base, relative.Value().View(), style);
    REQUIRE(reconstructed);
    auto normalized = Path::Normalize(allocator, path, style);
    REQUIRE(normalized);
    CHECK(reconstructed.Value().View() == normalized.Value().View());
}

Result<String> RunOwnedPathOperation(Allocator& allocator, StringView source, PathStyle style,
                                     uint32 operation) noexcept {
    const StringView base = style == PathStyle::Windows ? "C:/game/maps" : "/game/maps";
    switch (operation) {
    case 0:
        return Path::Normalize(allocator, source, style);
    case 1:
        return Path::Join(allocator, source, "child/../item", style);
    case 2:
        return Path::MakeAbsolute(allocator, base, "../level/한글", style);
    case 3:
        return Path::MakeRelative(allocator, base, source, style);
    case 4:
        return Path::ChangeExtension(allocator, source, "texture", style);
    case 5:
        return Path::SetExtension(allocator, source, "texture", style);
    case 6:
        return Path::NormalizeFilename(allocator, source, style);
    default:
        return Path::CollapseRelativeDirectories(allocator, source, style);
    }
}

} // namespace

TEST(PathNormalizeBothSyntaxesAndIdempotence) {
    constexpr NormalizeCase cases[] = {
        {"", "", PathStyle::Posix},
        {"/", "/", PathStyle::Posix},
        {"/maps//a/./b/../c/", "/maps/a/c", PathStyle::Posix},
        {"assets/../maps/level", "maps/level", PathStyle::Posix},
        {"../../assets/../level", "../../level", PathStyle::Posix},
        {"/한글/🎮/../맵", "/한글/맵", PathStyle::Posix},
        {"", "", PathStyle::Windows},
        {"C:\\", "C:/", PathStyle::Windows},
        {"C:\\maps\\a\\.\\b\\..\\c\\", "C:/maps/a/c", PathStyle::Windows},
        {"C:/maps//level", "C:/maps/level", PathStyle::Windows},
        {"assets\\..\\maps\\level", "maps/level", PathStyle::Windows},
        {"../../assets/../level", "../../level", PathStyle::Windows},
    };
    SystemAllocator allocator;
    for (const auto& sample : cases) {
        CheckNormalized(allocator, sample);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathJoinBothSyntaxesRejectsRootedRightHandSide) {
    SystemAllocator allocator;
    CheckJoin(allocator, "/game/maps", "level/./terrain", PathStyle::Posix,
              "/game/maps/level/terrain");
    CheckJoin(allocator, "C:\\game\\maps", "level\\terrain", PathStyle::Windows,
              "C:/game/maps/level/terrain");
    CHECK(!Path::Join(allocator, "/game", "/elsewhere", PathStyle::Posix));
    CHECK(!Path::Join(allocator, "C:/game", "D:/elsewhere", PathStyle::Windows));
    CHECK(!Path::Join(allocator, "C:/game", "D:elsewhere", PathStyle::Windows));
    CHECK(!Path::Join(allocator, "C:/game", "\\elsewhere", PathStyle::Windows));
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathAbsoluteAndRelativeRoundTrip) {
    SystemAllocator allocator;
    CheckRelativeRoundTrip(allocator, "/game/maps", "/game/assets/한글", PathStyle::Posix,
                           "../assets/한글");
    CheckRelativeRoundTrip(allocator, "C:/game/maps", "C:/game/assets/level", PathStyle::Windows,
                           "../assets/level");
    auto same = Path::MakeRelative(allocator, "/game/maps", "/game/maps", PathStyle::Posix);
    REQUIRE(same);
    CHECK(same.Value().View() == StringView("."));
    CHECK(!Path::MakeAbsolute(allocator, "relative/base", "item", PathStyle::Posix));
    CHECK(!Path::MakeRelative(allocator, "C:/game", "D:/game/item", PathStyle::Windows));
    CHECK(!Path::MakeAbsolute(allocator, "/", "../escape", PathStyle::Posix));
    CHECK(!Path::MakeAbsolute(allocator, "C:/", "../escape", PathStyle::Windows));
}

TEST(PathPartsUseBorrowedViewsAndExtensionExcludesDot) {
    const StringView file("/assets/한글.mesh");
    auto filename = Path::FileName(file, PathStyle::Posix);
    auto parent = Path::Parent(file, PathStyle::Posix);
    auto extension = Path::Extension(file, PathStyle::Posix);
    REQUIRE(filename);
    REQUIRE(parent);
    REQUIRE(extension);
    CHECK(filename.Value() == StringView("한글.mesh"));
    CHECK(parent.Value() == StringView("/assets"));
    CHECK(extension.Value() == StringView("mesh"));
    CHECK(filename.Value().Data() >= file.Data());
    CHECK(filename.Value().Data() < file.Data() + file.ByteCount());
    auto hidden = Path::Extension("/assets/.gitignore", PathStyle::Posix);
    REQUIRE(hidden);
    CHECK(hidden.Value() == StringView("gitignore"));
    auto multi = Path::Extension("C:\\assets\\archive.tar.gz", PathStyle::Windows);
    REQUIRE(multi);
    CHECK(multi.Value() == StringView("gz"));
}

TEST(PathContainmentComparesSegmentsAndWindowsCaseIsExplicit) {
    SystemAllocator allocator;
    auto equal = Path::IsUnder(allocator, "/game/maps", "/game/maps", PathStyle::Posix);
    REQUIRE(equal);
    CHECK(equal.Value());
    auto child = Path::IsUnder(allocator, "/game/maps/level", "/game/maps", PathStyle::Posix);
    REQUIRE(child);
    CHECK(child.Value());
    auto prefix = Path::IsUnder(allocator, "/game/maps2/level", "/game/maps", PathStyle::Posix);
    REQUIRE(prefix);
    CHECK(!prefix.Value());
    auto differentCase = Path::IsUnder(allocator, "C:/Game/maps", "c:/game", PathStyle::Windows);
    REQUIRE(differentCase);
    CHECK(!differentCase.Value());
    auto driveCase = Path::IsUnder(allocator, "C:/game/maps", "c:/game", PathStyle::Windows);
    REQUIRE(driveCase);
    CHECK(driveCase.Value());
}

TEST(PathRejectsMalformedUtf8EmbeddedNulAndRootEscape) {
    SystemAllocator allocator;
    constexpr char embedded[] = {'a', '\0', 'b'};
    const StringView badNul(embedded, sizeof(embedded));
    const StringView badUtf8("x\xFF", 2);
    const PathStyle styles[] = {PathStyle::Windows, PathStyle::Posix};
    const StringView invalidPaths[] = {badNul, badUtf8};
    for (PathStyle style : styles) {
        for (StringView bad : invalidPaths) {
            auto normalized = Path::Normalize(allocator, bad, style);
            CHECK(!normalized);
            CHECK(normalized.Error() == ErrorCode::InvalidArgument);
            CHECK(!Path::FileName(bad, style));
            CHECK(!Path::Extension(bad, style));
            CHECK(!Path::Join(allocator, "base", bad, style));
        }
    }
    auto posixEscape = Path::Normalize(allocator, "/../../item", PathStyle::Posix);
    auto windowsEscape = Path::Normalize(allocator, "C:/../../item", PathStyle::Windows);
    CHECK(!posixEscape && posixEscape.Error() == ErrorCode::InvalidArgument);
    CHECK(!windowsEscape && windowsEscape.Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathAllocationFailuresKeepSourceAndReleaseEveryAllocation) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        String source(system);
        REQUIRE(source.TryAssign(style == PathStyle::Windows ? "C:/game/assets/한글/../level.mesh"
                                                             : "/game/assets/한글/../level.mesh"));
        const char* original = source.Data();
        const StringView originalView = source.View();
        const auto baseline = system.Stats().liveAllocations;
        for (uint32 operation = 0; operation < 8; ++operation) {
            failing.DisableFailure();
            {
                auto completed = RunOwnedPathOperation(failing, source.View(), style, operation);
                REQUIRE(completed);
            }
            const uint64 attempts = failing.Attempts();
            CHECK(system.Stats().liveAllocations == baseline);
            for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
                failing.FailOnAttempt(attempt);
                auto completed = RunOwnedPathOperation(failing, source.View(), style, operation);
                CHECK(!completed);
                CHECK(completed.Error() == ErrorCode::OutOfMemory);
                CHECK(source.Data() == original);
                CHECK(source.View() == originalView);
                CHECK(system.Stats().liveAllocations == baseline);
            }
        }
        failing.DisableFailure();
        {
            auto contained = Path::IsUnder(
                failing, source.View(), style == PathStyle::Windows ? "C:/game" : "/game", style);
            REQUIRE(contained);
            CHECK(contained.Value());
        }
        const uint64 attempts = failing.Attempts();
        CHECK(system.Stats().liveAllocations == baseline);
        for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto contained = Path::IsUnder(
                failing, source.View(), style == PathStyle::Windows ? "C:/game" : "/game", style);
            CHECK(!contained);
            CHECK(contained.Error() == ErrorCode::OutOfMemory);
            CHECK(source.Data() == original);
            CHECK(source.View() == originalView);
            CHECK(system.Stats().liveAllocations == baseline);
        }
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(PathRootClassificationForBothSyntaxes) {
    struct RootCase {
        const char* input;
        PathStyle style;
        PathRoot expected;
    };
    constexpr RootCase cases[] = {
        {"", PathStyle::Posix, PathRoot::Relative},
        {"maps/level", PathStyle::Posix, PathRoot::Relative},
        {"/maps/level", PathStyle::Posix, PathRoot::PosixRoot},
        {"//server/share", PathStyle::Posix, PathRoot::PosixRoot},
        {"\\maps\\level", PathStyle::Posix, PathRoot::Relative},
        {"maps/level", PathStyle::Windows, PathRoot::Relative},
        {"C:maps/level", PathStyle::Windows, PathRoot::WindowsDriveRelative},
        {"C:/maps/level", PathStyle::Windows, PathRoot::WindowsDriveAbsolute},
        {"\\maps\\level", PathStyle::Windows, PathRoot::WindowsCurrentDriveRoot},
        {"\\\\server\\share\\maps", PathStyle::Windows, PathRoot::WindowsUnc},
    };
    for (const RootCase& sample : cases) {
        auto root = Path::Root(sample.input, sample.style);
        REQUIRE(root);
        CHECK(root.Value() == sample.expected);
    }
    auto incompleteUnc = Path::Root("//server", PathStyle::Windows);
    CHECK(!incompleteUnc);
    CHECK(incompleteUnc.Error() == ErrorCode::InvalidArgument);
    auto extendedPath = Path::Root("\\\\?\\C:\\file", PathStyle::Windows);
    CHECK(!extendedPath);
    CHECK(extendedPath.Error() == ErrorCode::Unsupported);
    auto device = Path::Root("\\\\.\\device", PathStyle::Windows);
    CHECK(!device);
    CHECK(device.Error() == ErrorCode::Unsupported);
}

TEST(PathPartBoundariesAndExtensionReplacement) {
    auto root = Path::Parent("/", PathStyle::Posix);
    auto trailingParent = Path::Parent("/assets/folder/", PathStyle::Posix);
    auto trailingName = Path::FileName("/assets/folder/", PathStyle::Posix);
    auto trailingDot = Path::Extension("file.", PathStyle::Posix);
    auto dot = Path::Extension(".", PathStyle::Posix);
    auto doubleDot = Path::Extension("..", PathStyle::Posix);
    REQUIRE(root && trailingParent && trailingName && trailingDot && dot && doubleDot);
    CHECK(root.Value() == StringView("/"));
    CHECK(trailingParent.Value() == StringView("/assets/folder"));
    CHECK(trailingName.Value().IsEmpty());
    CHECK(trailingDot.Value().IsEmpty());
    CHECK(dot.Value().IsEmpty());
    CHECK(doubleDot.Value().IsEmpty());

    SystemAllocator allocator;
    auto replace =
        Path::ChangeExtension(allocator, "/assets/file.tar.gz", ".zip", PathStyle::Posix);
    auto remove = Path::ChangeExtension(allocator, "/assets/file.tar.gz", "", PathStyle::Posix);
    auto hidden = Path::ChangeExtension(allocator, "/assets/.gitignore", "txt", PathStyle::Posix);
    REQUIRE(replace && remove && hidden);
    CHECK(replace.Value().View() == StringView("/assets/file.tar.zip"));
    CHECK(remove.Value().View() == StringView("/assets/file.tar"));
    CHECK(hidden.Value().View() == StringView("/assets/.txt"));
    auto unchangedDirectory = Path::ChangeExtension(allocator, "/assets/", "txt", PathStyle::Posix);
    REQUIRE(unchangedDirectory);
    CHECK(unchangedDirectory.Value().View() == StringView("/assets/"));
    CHECK(!Path::ChangeExtension(allocator, "/assets/file", "x/y", PathStyle::Posix));
}

TEST(PathNormalizationExhaustiveShortCombinationsRemainIdempotent) {
    const char* components[] = {"x", "..", ".", "", "한글", "🎮"};
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    SystemAllocator allocator;
    for (PathStyle style : styles) {
        for (const char* first : components) {
            for (const char* second : components) {
                for (const char* third : components) {
                    String input(allocator);
                    REQUIRE(input.TryAssign(style == PathStyle::Windows ? "C:/a/b/c/" : "/a/b/c/"));
                    REQUIRE(input.TryAppend(first));
                    REQUIRE(input.TryAppend("/"));
                    REQUIRE(input.TryAppend(second));
                    REQUIRE(input.TryAppend("/"));
                    REQUIRE(input.TryAppend(third));
                    auto normalized = Path::Normalize(allocator, input.View(), style);
                    REQUIRE(normalized);
                    auto again = Path::Normalize(allocator, normalized.Value().View(), style);
                    REQUIRE(again);
                    CHECK(normalized.Value().View() == again.Value().View());
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathWindowsNormalizationDoesNotCreateDriveRoot) {
    SystemAllocator allocator;
    auto absoluteLooking = Path::Normalize(allocator, "./C:/secret", PathStyle::Windows);
    auto driveRelativeLooking = Path::Normalize(allocator, "x/../D:foo", PathStyle::Windows);
    REQUIRE(absoluteLooking && driveRelativeLooking);
    CHECK(absoluteLooking.Value().View() == StringView("./C:/secret"));
    CHECK(driveRelativeLooking.Value().View() == StringView("./D:foo"));
    auto firstRoot = Path::Root(absoluteLooking.Value().View(), PathStyle::Windows);
    auto secondRoot = Path::Root(driveRelativeLooking.Value().View(), PathStyle::Windows);
    REQUIRE(firstRoot && secondRoot);
    CHECK(firstRoot.Value() == PathRoot::Relative);
    CHECK(secondRoot.Value() == PathRoot::Relative);

    auto joined = Path::Join(allocator, "C:/base", "./C:/secret", PathStyle::Windows);
    auto absolute = Path::MakeAbsolute(allocator, "C:/base", "./C:/secret", PathStyle::Windows);
    auto contained = Path::IsUnder(allocator, "./C:/secret", ".", PathStyle::Windows);
    REQUIRE(joined && absolute && contained);
    CHECK(joined.Value().View() == StringView("C:/base/C:/secret"));
    CHECK(absolute.Value().View() == StringView("C:/base/C:/secret"));
    CHECK(contained.Value());

    auto relative = Path::MakeRelative(allocator, "C:/base", "C:/base/D:foo", PathStyle::Windows);
    REQUIRE(relative);
    CHECK(relative.Value().View() == StringView("./D:foo"));
    auto relativeRoot = Path::Root(relative.Value().View(), PathStyle::Windows);
    REQUIRE(relativeRoot);
    CHECK(relativeRoot.Value() == PathRoot::Relative);
    auto reconstructed =
        Path::MakeAbsolute(allocator, "C:/base", relative.Value().View(), PathStyle::Windows);
    REQUIRE(reconstructed);
    CHECK(reconstructed.Value().View() == StringView("C:/base/D:foo"));
}

TEST(PathWindowsJoinOnlyTreatsLettersAsDriveRoots) {
    SystemAllocator allocator;
    auto joined = Path::Join(allocator, "1:", "foo", PathStyle::Windows);
    REQUIRE(joined);
    CHECK(joined.Value().View() == StringView("1:/foo"));
    auto root = Path::Root(joined.Value().View(), PathStyle::Windows);
    REQUIRE(root);
    CHECK(root.Value() == PathRoot::Relative);
}

TEST(PathRelativeContainmentDistinguishesParentTraversal) {
    SystemAllocator allocator;
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        auto equal = Path::IsUnder(allocator, "..", "..", style);
        auto child = Path::IsUnder(allocator, "../child", "..", style);
        auto parent = Path::IsUnder(allocator, "../..", "..", style);
        auto parentChild = Path::IsUnder(allocator, "../../child", "..", style);
        auto deeperChild = Path::IsUnder(allocator, "../../child", "../..", style);
        auto higherBase = Path::IsUnder(allocator, "../child", "../..", style);
        REQUIRE(equal && child && parent && parentChild && deeperChild && higherBase);
        CHECK(equal.Value());
        CHECK(child.Value());
        CHECK(!parent.Value());
        CHECK(!parentChild.Value());
        CHECK(deeperChild.Value());
        CHECK(higherBase.Value());
    }
}

TEST(PathWindowsRelativeResultWithColonRemainsRelativeAndRoundTrips) {
    SystemAllocator allocator;
    auto relative =
        Path::MakeRelative(allocator, "C:/base", "C:/base/a:stream", PathStyle::Windows);
    REQUIRE(relative);
    CHECK(relative.Value().View() == StringView("./a:stream"));
    auto root = Path::Root(relative.Value().View(), PathStyle::Windows);
    REQUIRE(root);
    CHECK(root.Value() == PathRoot::Relative);
    auto absolute =
        Path::MakeAbsolute(allocator, "C:/base", relative.Value().View(), PathStyle::Windows);
    REQUIRE(absolute);
    CHECK(absolute.Value().View() == StringView("C:/base/a:stream"));
}

TEST(PathRelativeContainmentIncludesCurrentDirectoryUnderParents) {
    SystemAllocator allocator;
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        auto current = Path::IsUnder(allocator, ".", "..", style);
        auto child = Path::IsUnder(allocator, "child", "..", style);
        auto unknown = Path::IsUnder(allocator, "../child", "../../name", style);
        REQUIRE(current && child && unknown);
        CHECK(current.Value());
        CHECK(child.Value());
        CHECK(!unknown.Value());
    }
}

TEST(PathExtensionChangeAndSetKeepOriginalSpelling) {
    SystemAllocator allocator;
    struct Case {
        PathStyle style;
        const char* path;
        const char* extension;
        const char* changed;
        const char* assigned;
    };
    const Case cases[] = {
        {PathStyle::Posix, "/Assets/FILE", "txt", "/Assets/FILE", "/Assets/FILE.txt"},
        {PathStyle::Posix, "/a/.hidden", ".new", "/a/.new", "/a/.new"},
        {PathStyle::Posix, "/a/file.", "", "/a/file", "/a/file"},
        {PathStyle::Posix, "/a/file.", "txt", "/a/file.txt", "/a/file.txt"},
        {PathStyle::Posix, "/a/file", "", "/a/file", "/a/file"},
        {PathStyle::Windows, "c:\\Mixed\\File", ".TXT", "c:\\Mixed\\File", "c:\\Mixed\\File.TXT"},
        {PathStyle::Windows, "c:\\Mixed\\FILE.TXT", "mesh", "c:\\Mixed\\FILE.mesh",
         "c:\\Mixed\\FILE.mesh"},
        {PathStyle::Windows, "c:\\Mixed\\.hidden", "", "c:\\Mixed\\", "c:\\Mixed\\"},
    };
    for (const Case& sample : cases) {
        auto changed =
            Path::ChangeExtension(allocator, sample.path, sample.extension, sample.style);
        auto assigned = Path::SetExtension(allocator, sample.path, sample.extension, sample.style);
        REQUIRE(changed && assigned);
        CHECK(changed.Value().View() == StringView(sample.changed));
        CHECK(assigned.Value().View() == StringView(sample.assigned));
    }
    CHECK(!Path::ChangeExtension(allocator, "file", "invalid/name", PathStyle::Posix));
    auto dotOnly = Path::SetExtension(allocator, "file", ".", PathStyle::Posix);
    auto directory = Path::SetExtension(allocator, "folder/", "txt", PathStyle::Posix);
    REQUIRE(dotOnly && directory);
    CHECK(dotOnly.Value().View() == StringView("file."));
    CHECK(directory.Value().View() == StringView("folder/.txt"));
}

TEST(PathSplitAndBaseFileNameReturnUnmodifiedBorrowedViews) {
    struct Case {
        PathStyle style;
        const char* input;
        const char* directory;
        const char* name;
        const char* extension;
    };
    const Case cases[] = {
        {PathStyle::Posix, "A/B/C.D", "A/B", "C", "D"},
        {PathStyle::Posix, "A/B/C", "A/B", "C", ""},
        {PathStyle::Posix, "A/B/", "A/B", "", ""},
        {PathStyle::Posix, "A/B/.D", "A/B", "", "D"},
        {PathStyle::Posix, "A/B.C.D", "A", "B.C", "D"},
        {PathStyle::Posix, "A", "", "A", ""},
        {PathStyle::Posix, ".D", "", "", "D"},
        {PathStyle::Posix, "A/B/file.", "A/B", "file", ""},
        {PathStyle::Posix, "A/B/..", "A/B", "..", ""},
        {PathStyle::Windows, "C:\\Data\\archive.tar.gz", "C:\\Data", "archive.tar", "gz"},
    };
    for (const Case& sample : cases) {
        const StringView input(sample.input);
        auto parts = Path::Split(input, sample.style);
        auto base = Path::BaseFileName(input, sample.style);
        REQUIRE(parts && base);
        CHECK(parts.Value().directory == StringView(sample.directory));
        CHECK(parts.Value().name == StringView(sample.name));
        CHECK(parts.Value().extension == StringView(sample.extension));
        if (StringView(sample.input) == StringView("A/B/..")) {
            CHECK(base.Value() == StringView("."));
        } else {
            CHECK(base.Value() == parts.Value().name);
        }
        CHECK(parts.Value().name.Data() >= input.Data());
        CHECK(parts.Value().name.Data() <= input.Data() + input.ByteCount());
    }
    constexpr char nul[] = {'a', '\0', 'b'};
    CHECK(!Path::Split(StringView(nul, sizeof(nul)), PathStyle::Posix));
    CHECK(!Path::BaseFileName(StringView("a\xFF", 2), PathStyle::Windows));
}

TEST(PathUe56DirectoryAndSplitSeparateFromParent) {
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        auto directory = Path::Directory("/file", style);
        auto parent = Path::Parent("/file", style);
        auto split = Path::Split("/file", style);
        auto dot = Path::Split(".", style);
        auto dotDot = Path::Split("..", style);
        auto baseDot = Path::BaseFileName(".", style);
        auto baseDotDot = Path::BaseFileName("..", style);
        REQUIRE(directory && parent && split && dot && dotDot && baseDot && baseDotDot);
        CHECK(directory.Value().IsEmpty());
        CHECK(parent.Value() == StringView("/"));
        CHECK(split.Value().directory.IsEmpty());
        CHECK(split.Value().name == StringView("file"));
        CHECK(dot.Value().name.IsEmpty());
        CHECK(dotDot.Value().name == StringView(".."));
        CHECK(baseDot.Value().IsEmpty());
        CHECK(baseDotDot.Value() == StringView("."));
    }
    auto drive = Path::Directory("C:/file", PathStyle::Windows);
    auto driveSplit = Path::Split("C:/file", PathStyle::Windows);
    auto driveParent = Path::Parent("C:/file", PathStyle::Windows);
    auto uncName = Path::FileName("//server/share", PathStyle::Windows);
    auto uncSplit = Path::Split("//server/share", PathStyle::Windows);
    auto driveRelativeName = Path::FileName("C:foo", PathStyle::Windows);
    REQUIRE(drive && driveSplit && driveParent && uncName && uncSplit && driveRelativeName);
    CHECK(drive.Value() == StringView("C:"));
    CHECK(driveSplit.Value().directory == StringView("C:"));
    CHECK(driveParent.Value() == StringView("C:/"));
    CHECK(uncName.Value() == StringView("share"));
    CHECK(uncSplit.Value().directory == StringView("//server"));
    CHECK(uncSplit.Value().name == StringView("share"));
    CHECK(driveRelativeName.Value() == StringView("C:foo"));
}

TEST(PathUe56DotExtensionAndDirectoryName) {
    SystemAllocator allocator;
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        auto dot = Path::SetExtension(allocator, "file", ".", style);
        auto replaced = Path::ChangeExtension(allocator, "file.ext", ".", style);
        auto folder = Path::SetExtension(allocator, "folder/", "txt", style);
        auto unchanged = Path::ChangeExtension(allocator, "folder/", "txt", style);
        auto empty = Path::SetExtension(allocator, "", "txt", style);
        REQUIRE(dot && replaced && folder && unchanged && empty);
        CHECK(dot.Value().View() == StringView("file."));
        CHECK(replaced.Value().View() == StringView("file."));
        CHECK(folder.Value().View() == StringView("folder/.txt"));
        CHECK(unchanged.Value().View() == StringView("folder/"));
        CHECK(empty.Value().View() == StringView(".txt"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PathUe56FilenameNormalizationIsIndependentFromFullNormalization) {
    SystemAllocator allocator;
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        auto result = Path::NormalizeFilename(allocator, "C:\\A\\..\\한글//", style);
        REQUIRE(result);
        CHECK(result.Value().View() == StringView("C:/A/../한글//"));
        auto empty = Path::NormalizeFilename(allocator, "", style);
        REQUIRE(empty);
        CHECK(empty.Value().IsEmpty());
    }
    auto posix = Path::Normalize(allocator, "a\\b", PathStyle::Posix);
    REQUIRE(posix);
    CHECK(posix.Value().View() == StringView("a\\b"));
}

TEST(PathUe56CollapseRelativeDirectoriesAndFailurePreserveInput) {
    SystemAllocator allocator;
    struct Case {
        const char* input;
        const char* expected;
    };
    const Case valid[] = {{"", ""},           {"a/./b", "a/b"},      {"a/b/../c", "a/c"},
                          {"/a/..", ""},      {"a/..", ""},          {"C:/a/..", "C:"},
                          {"a.../x", "a..x"}, {"a/..foo", "a/..foo"}};
    const char* invalid[] = {"../file", "/../file", "..hidden/file", "C:/..", "a/../../file"};
    const PathStyle styles[] = {PathStyle::Posix, PathStyle::Windows};
    for (PathStyle style : styles) {
        for (const Case& sample : valid) {
            auto value = Path::CollapseRelativeDirectories(allocator, sample.input, style);
            REQUIRE(value);
            CHECK(value.Value().View() == StringView(sample.expected));
        }
        for (const char* sample : invalid) {
            String source(allocator);
            REQUIRE(source.TryAssign(sample));
            const char* before = source.Data();
            const StringView view = source.View();
            auto failed = Path::CollapseRelativeDirectories(allocator, source.View(), style);
            CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
            CHECK(source.Data() == before && source.View() == view);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
