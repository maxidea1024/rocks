#include "AssetDatabaseTestSupport.h"

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Text/Path.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 프로젝트 배치 시험이다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a): 디렉터리 이름,
// 구분자 정리, 논리 경로 ↔ 파일 경로 왕복, 패키지 파일이 아닌 이름, 비 NFC 파일 이름, OS 경로 상한,
// 할당 실패.
using namespace rocks;

namespace project_layout_tests_file {

namespace {

// 플랫폼의 절대 프로젝트 루트 예다(디스크는 보지 않는다).
constexpr const char* AbsoluteDemoRoot = IsWindows ? "D:/Games/Demo" : "/Games/Demo";
// 절대 루트를 만드는 접두다.
constexpr const char* AbsolutePrefix = IsWindows ? "C:/" : "/";

} // namespace

// 디렉터리(4.2절, UE와 같은 이름): 절대 루트는 정규화하고 구분자를 '/'로 쓰며 끝 구분자는 뺀다.
// 루트뿐인 경로("C:/"·"/")는 그대로 두고 자식에 구분자를 겹치지 않는다. 상대 루트는 현재 작업
// 디렉터리 기준 절대 경로가 된다(UE ConvertRelativePathToFull). 빈 루트·Windows 현재 드라이브 루트·
// 드라이브 상대는 InvalidArgument이고 이전 값을 그대로 둔다.
TEST(ProjectLayout_Directories) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    if constexpr (IsWindows) {
        REQUIRE(layout.Initialize("C:\\Projects\\MyGame\\\\"));
        CHECK(layout.Root() == "C:/Projects/MyGame");
        CHECK(layout.ContentDirectory() == "C:/Projects/MyGame/Content");
        CHECK(layout.ConfigDirectory() == "C:/Projects/MyGame/Config");
        CHECK(layout.IntermediateDirectory() == "C:/Projects/MyGame/Intermediate");
        CHECK(layout.SavedDirectory() == "C:/Projects/MyGame/Saved");
        CHECK(layout.Initialize("/Projects").Error() == ErrorCode::InvalidArgument);
        CHECK(layout.Initialize("C:Projects").Error() == ErrorCode::InvalidArgument);
        CHECK(layout.Root() == "C:/Projects/MyGame");
        REQUIRE(layout.Initialize("C:/"));
        CHECK(layout.Root() == "C:/" && layout.ContentDirectory() == "C:/Content");
    } else {
        REQUIRE(layout.Initialize("/Projects//MyGame/"));
        CHECK(layout.Root() == "/Projects/MyGame");
        CHECK(layout.ContentDirectory() == "/Projects/MyGame/Content");
        CHECK(layout.IntermediateDirectory() == "/Projects/MyGame/Intermediate");
        REQUIRE(layout.Initialize("/"));
        CHECK(layout.Root() == "/" && layout.ContentDirectory() == "/Content");
    }

    String previous(allocator);
    REQUIRE(previous.TryAppend(layout.Root()));
    CHECK(layout.Initialize("").Error() == ErrorCode::InvalidArgument);
    // 경로로 해석할 수 없는 루트(NUL — Core Path가 InvalidArgument, Windows 장치 이름 공간 —
    // Unsupported)는 그 오류이고 이전 값을 그대로 둔다.
    CHECK(layout.Initialize(StringView("Pro\0ject", 8)).Error() == ErrorCode::InvalidArgument);
    if constexpr (IsWindows) {
        CHECK(layout.Initialize("\\\\?\\C:\\Projects").Error() == ErrorCode::Unsupported);
    }
    CHECK(layout.Root() == previous.View());
    REQUIRE(layout.Initialize("relative/project"));
    CHECK(Path::Root(layout.Root()).Value() != PathRoot::Relative);
    CHECK(layout.Root().EndsWith("/relative/project", StringComparison::CaseSensitive));
    CHECK(layout.IntermediateDirectory().EndsWith("/relative/project/Intermediate",
                                                  StringComparison::CaseSensitive));
}

// Initialize 전(7.1절 10): 배치가 비었으면 파일 경로를 만들지 않는다(InvalidArgument — 파일 시스템
// 루트 아래 경로가 되지 않게).
TEST(ProjectLayout_Uninitialized) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    auto path = ParseLogicalPath(allocator, "/Game/Hero");
    REQUIRE(path);
    String file(allocator);
    CHECK(layout.TryMakeFilePath(path.Value(), file).Error() == ErrorCode::InvalidArgument);
    CHECK(file.View().IsEmpty());
}

// 왕복(7.1절 10): 논리 경로의 새 파일 경로는 "<Content>/<Relative>.rasset"이고, Content 상대
// 이름에서 같은 논리 경로가 나온다(마운트 표기는 정규 철자).
TEST(ProjectLayout_PathRoundTrip) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(AbsoluteDemoRoot));
    const char* const paths[] = {"/Game/Hero", "/game/Characters/Hero",
                                 "/Game/\xed\x95\x9c\xea\xb8\x80/A_b-9"};
    for (const char* text : paths) {
        auto path = ParseLogicalPath(allocator, text);
        REQUIRE(path);
        String file(allocator);
        REQUIRE(layout.TryMakeFilePath(path.Value(), file));
        String prefixText(allocator);
        REQUIRE(prefixText.TryAppend(AbsoluteDemoRoot));
        REQUIRE(prefixText.TryAppend("/Content/"));
        const StringView prefix = prefixText.View();
        REQUIRE(file.View().StartsWith(prefix));
        const StringView relative(file.View().Data() + prefix.ByteCount(),
                                  file.View().ByteCount() - prefix.ByteCount());
        CHECK(relative.EndsWith(EditorPackageExtension));
        auto back = layout.TryMakeLogicalPath(relative);
        REQUIRE(back);
        CHECK(back.Value().Text() == path.Value().Text());
    }
}

// 패키지 파일이 아닌 이름(7.1절 10): 확장자가 정확히 ".rasset"이 아니면(대소문자 다름·임시 파일·
// 확장자뿐) InvalidArgument이고 사유는 None이다. 규칙을 어긴 이름은 그 사유다.
TEST(ProjectLayout_NonPackageNames) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize("P"));
    const char* const notPackages[] = {
        "Hero.rasset.0123.tmp", ".rasset", ".RASSET", "Hero", "Hero.rpkg", ""};
    for (const char* name : notPackages) {
        LogicalPathFailure failure;
        failure.error = LogicalPathError::TooLong;
        auto made = layout.TryMakeLogicalPath(name, &failure);
        CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.error == LogicalPathError::None);
    }

    // 확장자는 ASCII 대소문자를 무시한다(UE와 같다 — 그 이름도 같은 패키지 자리다).
    auto upper = layout.TryMakeLogicalPath("Dir/Hero.RASSET");
    REQUIRE(upper);
    CHECK(upper.Value().Text() == "/Game/Dir/Hero");
    CHECK(layout.TryMakeLogicalPath("Dir/Hero.RaSsEt"));
    LogicalPathFailure failure;
    CHECK(!layout.TryMakeLogicalPath("Dir/He ro.rasset", &failure));
    CHECK(failure.error == LogicalPathError::InvalidComponent && failure.component == 2);
    CHECK(!layout.TryMakeLogicalPath("Dir//Hero.rasset", &failure));
    CHECK(failure.error == LogicalPathError::EmptyComponent);
}

// 비 NFC 파일 이름(7.1절 10): 열거한 이름이 NFC가 아니면 byte 비교 파일
// 시스템(Windows·Linux·Android) 에서는 NotNfcOnDisk로 거부하고, 이름을 정규화하는 파일
// 시스템(macOS)에서는 NFC 논리 경로로 받는다.
TEST(ProjectLayout_NonNfcFileNames) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize("P"));
    LogicalPathFailure failure;
    auto made = layout.TryMakeLogicalPath("Cafe\xcc\x81.rasset", &failure);
    if constexpr (FileSystemNormalizesNames) {
        REQUIRE(made);
        CHECK(made.Value().Text() == "/Game/Caf\xc3\xa9");
    } else {
        CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.error == LogicalPathError::NotNfcOnDisk);
    }

    // 겹친 위반(확장자 → 파싱 → NotNfc 순서): 비 NFC이면서 규칙을 어기면 InvalidComponent, 비
    // NFC이면서 너무 길면 TooLong이다. 디렉터리 구성 요소만 비 NFC여도 같은 판정이다.
    CHECK(!layout.TryMakeLogicalPath("Cafe\xcc\x81 x.rasset", &failure));
    CHECK(failure.error == LogicalPathError::InvalidComponent);
    String nfdLong(allocator);
    // 일곱 구성 요소(각 NFD 60 byte → NFC 40 byte): NFC 전체 5 + 7 × 41 = 292 byte라 TooLong이다.
    for (uint32 component = 0; component < 7; ++component) {
        for (uint32 index = 0; index < 20; ++index) {
            REQUIRE(nfdLong.TryAppend("e\xcc\x81"));
        }

        REQUIRE(nfdLong.TryAppend(component + 1 < 7 ? "/" : ".rasset"));
    }

    CHECK(!layout.TryMakeLogicalPath(nfdLong.View(), &failure));
    CHECK(failure.error == LogicalPathError::TooLong);
    auto directory = layout.TryMakeLogicalPath("Cafe\xcc\x81/Hero.rasset", &failure);
    if constexpr (FileSystemNormalizesNames) {
        CHECK(directory);
    } else {
        CHECK(!directory && failure.error == LogicalPathError::NotNfcOnDisk);
    }

    // NFC 이름은 어느 쪽이든 받는다. 사유 자리가 없어도 같은 판정이다.
    CHECK(layout.TryMakeLogicalPath("Caf\xc3\xa9.rasset"));
    CHECK(static_cast<bool>(layout.TryMakeLogicalPath("Cafe\xcc\x81.rasset")) ==
          FileSystemNormalizesNames);
}

// OS 경로 상한(7.1절 10): 임시 접미사(41 byte)까지 붙인 절대 경로가 플랫폼 상한을 넘으면
// CapacityExceeded이고 out은 그대로다. 상한과 같으면 받는다.
TEST(ProjectLayout_NativePathLimit) {
    SystemAllocator allocator;
    auto path = ParseLogicalPath(allocator, "/Game/Hero");
    REQUIRE(path);
    // 경로 = 루트 + "/Content/" + "Hero" + ".rasset" + 접미사.
    const usize fixed = 9 + 4 + EditorPackageExtension.ByteCount() + TemporaryFileSuffixBytes;
    const usize rootBytes = MaxNativePathBytes() - fixed;
    String root(allocator);
    REQUIRE(root.TryAppend(AbsolutePrefix));
    while (root.View().ByteCount() < rootBytes) {
        REQUIRE(root.TryAppend("r"));
    }
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(root.View()));
    String file(allocator);
    REQUIRE(layout.TryMakeFilePath(path.Value(), file));
    CHECK(file.View().ByteCount() + TemporaryFileSuffixBytes == MaxNativePathBytes());
    REQUIRE(root.TryAppend("r"));
    REQUIRE(layout.Initialize(root.View()));
    String unchanged(allocator);
    REQUIRE(unchanged.TryAppend("keep"));
    CHECK(layout.TryMakeFilePath(path.Value(), unchanged).Error() == ErrorCode::CapacityExceeded);
    CHECK(unchanged.View() == "keep");
}

// 할당 실패(7.1절 10): Initialize·TryMakeFilePath·TryMakeLogicalPath의 모든 할당 지점에서 한 번
// 실패하면 그 오류이고(Initialize는 이전 값 그대로), 실패가 없으면 같은 결과다. 루트와 Content 상대
// 이름은 임시 저장소의 인라인 크기(512 byte)를 넘겨 자라는 경로까지 지난다(긴 상대 이름은 저장소를
// 키운 뒤 TooLong으로 거부된다).
TEST(ProjectLayout_AllocationFailures) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    // 루트 600 byte 넘게, Content 상대 이름 640 byte 넘게(구성 요소 10개 × 64)다.
    String longRoot(system);
    REQUIRE(longRoot.TryAppend(AbsolutePrefix));
    while (longRoot.View().ByteCount() < 600) {
        REQUIRE(longRoot.TryAppend("Dir/"));
    }

    REQUIRE(longRoot.TryAppend("Root"));
    String longRelative(system);
    constexpr uint32 LongComponents = 10;
    for (uint32 component = 0; component < LongComponents; ++component) {
        for (uint32 byte = 0; byte < MaxNameComponentBytes; ++byte) {
            REQUIRE(longRelative.TryAppend("g"));
        }

        REQUIRE(longRelative.TryAppend(component + 1 < LongComponents ? "/" : ".rasset"));
    }

    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        ProjectLayout layout(failing);
        REQUIRE(layout.Initialize("Old"));
        String oldRoot(system);
        REQUIRE(oldRoot.TryAppend(layout.Root()));
        auto path = ParseLogicalPath(system, "/Game/Some/Deep/Path/Name");
        REQUIRE(path);
        String file(failing);
        failing.FailOnAttempt(attempt);
        const auto initialized = layout.Initialize(longRoot.View());
        // 상대 루트는 작업 디렉터리 버퍼와 절대 경로를 만든다(그 할당도 실패시킨다).
        const auto relative =
            initialized ? layout.Initialize("Relative/Root") : Result<void>::Success();
        const auto restored =
            initialized && relative ? layout.Initialize(longRoot.View()) : Result<void>::Success();
        const auto filed = initialized ? layout.TryMakeFilePath(path.Value(), file)
                                       : Result<void>::Failure(ErrorCode::OutOfMemory);
        auto logical = layout.TryMakeLogicalPath("Some/Deep/Path/Name.rasset");
        LogicalPathFailure longFailure;
        auto longLogical = layout.TryMakeLogicalPath(longRelative.View(), &longFailure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(initialized && relative && restored && filed && logical);
            CHECK(file.View().EndsWith("/Content/Some/Deep/Path/Name.rasset"));
            CHECK(file.View().StartsWith(longRoot.View()));
            CHECK(logical.Value().Text() == "/Game/Some/Deep/Path/Name");
            CHECK(!longLogical && longFailure.error == LogicalPathError::TooLong);
            break;
        }

        ++failures;
        if (!initialized) {
            CHECK(initialized.Error() == ErrorCode::OutOfMemory);
            CHECK(layout.Root() == oldRoot.View());
        } else if (!relative || !restored) {
            CHECK((!relative ? relative : restored).Error() == ErrorCode::OutOfMemory);
            // 실패한 Initialize는 바로 앞의 값을 그대로 둔다.
            if (!relative) {
                CHECK(layout.Root() == longRoot.View());
            } else {
                CHECK(layout.Root().EndsWith("/Relative/Root", StringComparison::CaseSensitive));
            }
        } else if (!filed) {
            CHECK(filed.Error() == ErrorCode::OutOfMemory && file.View().IsEmpty());
        } else if (!logical) {
            CHECK(logical.Error() == ErrorCode::OutOfMemory);
        } else {
            CHECK(!longLogical && longLogical.Error() == ErrorCode::OutOfMemory);
            CHECK(longFailure.error == LogicalPathError::None);
        }
    }

    CHECK(failures > 0);
}

// OS 경로 상한의 실제 경계(7.1절 10): MaxNativePathBytes byte인 절대 경로의 파일을 실제로 만들 수
// 있다(상한이 OS가 받는 범위 안이다). 경로는 시험 프로젝트 아래 200 byte 디렉터리를 이어 만든다.
TEST(ProjectLayout_NativePathLimitOnDisk) {
    asset_database_test::TempProject project("path-limit");
    REQUIRE(project.made);
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize(project.Root()));
    constexpr usize DirectoryBytes = 200;
    String path(allocator);
    REQUIRE(path.TryAppend(layout.Root()));
    // 디렉터리 부분을 정확히 (상한 − 1 − FileNameBytes) byte로 만든다(구성 요소는 200 byte 이하 —
    // 파일 시스템 이름 한도 255 안). 남은 길이가 구분자 하나뿐이 되지 않게 마지막 조각을 줄인다.
    constexpr usize FileNameBytes = 100;
    const usize directoryBytes = MaxNativePathBytes() - 1 - FileNameBytes;
    while (path.View().ByteCount() < directoryBytes) {
        usize chunk = directoryBytes - path.View().ByteCount() - 1;
        chunk = chunk > DirectoryBytes ? DirectoryBytes : chunk;
        if (directoryBytes - path.View().ByteCount() - 1 - chunk == 1) {
            --chunk;
        }

        REQUIRE(path.TryAppend("/"));
        for (usize index = 0; index < chunk; ++index) {
            REQUIRE(path.TryAppend("d"));
        }
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    REQUIRE(platform::MakeDirectoryTree(files, path.View().Data(), path.View().ByteCount()));
    REQUIRE(path.TryAppend("/"));
    while (path.View().ByteCount() < MaxNativePathBytes()) {
        REQUIRE(path.TryAppend("f"));
    }

    REQUIRE(path.View().ByteCount() == MaxNativePathBytes());
    platform::File file;
    CHECK(file.Open(files, path.View().Data(), path.View().ByteCount(),
                    platform::FileOpenMode::CreateNew));
    CHECK(file.Close());
}

} // namespace project_layout_tests_file
