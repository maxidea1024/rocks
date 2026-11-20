#include "AssetDatabaseTestSupport.h"

#include <AssetDatabase/ProjectLayout.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 프로젝트 배치 시험이다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a): 디렉터리 이름,
// 구분자 정리, 논리 경로 ↔ 파일 경로 왕복, 패키지 파일이 아닌 이름, 비 NFC 파일 이름, OS 경로 상한,
// 할당 실패.
using namespace rocks;

namespace project_layout_tests_file {

// 디렉터리(4.2절, UE와 같은 이름): '\'는 '/'로, 끝 구분자는 빼며, 루트가 "/"면 자식에 구분자를
// 겹치지 않는다. 빈 루트는 InvalidArgument이고 이전 값을 그대로 둔다.
TEST(ProjectLayout_Directories) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize("C:\\Projects\\MyGame\\\\"));
    CHECK(layout.Root() == "C:/Projects/MyGame");
    CHECK(layout.ContentDirectory() == "C:/Projects/MyGame/Content");
    CHECK(layout.ConfigDirectory() == "C:/Projects/MyGame/Config");
    CHECK(layout.IntermediateDirectory() == "C:/Projects/MyGame/Intermediate");
    CHECK(layout.SavedDirectory() == "C:/Projects/MyGame/Saved");
    CHECK(layout.Initialize("").Error() == ErrorCode::InvalidArgument);
    CHECK(layout.Root() == "C:/Projects/MyGame");
    REQUIRE(layout.Initialize("/"));
    CHECK(layout.Root() == "/" && layout.ContentDirectory() == "/Content");
    REQUIRE(layout.Initialize("relative/project"));
    CHECK(layout.IntermediateDirectory() == "relative/project/Intermediate");
}

// 왕복(7.1절 10): 논리 경로의 새 파일 경로는 "<Content>/<Relative>.rasset"이고, Content 상대
// 이름에서 같은 논리 경로가 나온다(마운트 표기는 정규 철자).
TEST(ProjectLayout_PathRoundTrip) {
    SystemAllocator allocator;
    ProjectLayout layout(allocator);
    REQUIRE(layout.Initialize("D:/Games/Demo"));
    const char* const paths[] = {"/Game/Hero", "/game/Characters/Hero",
                                 "/Game/\xed\x95\x9c\xea\xb8\x80/A_b-9"};
    for (const char* text : paths) {
        auto path = ParseLogicalPath(allocator, text);
        REQUIRE(path);
        String file(allocator);
        REQUIRE(layout.TryMakeFilePath(path.Value(), file));
        const StringView prefix = "D:/Games/Demo/Content/";
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
        "Hero.RASSET", "Hero.rasset.0123.tmp", ".rasset", "Hero", "Hero.rpkg", ""};
    for (const char* name : notPackages) {
        LogicalPathFailure failure;
        failure.error = LogicalPathError::TooLong;
        auto made = layout.TryMakeLogicalPath(name, &failure);
        CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.error == LogicalPathError::None);
    }

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
    for (usize index = 0; index < rootBytes; ++index) {
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
    REQUIRE(longRoot.TryAppend("C:/"));
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
        auto path = ParseLogicalPath(system, "/Game/Some/Deep/Path/Name");
        REQUIRE(path);
        String file(failing);
        failing.FailOnAttempt(attempt);
        const auto initialized = layout.Initialize(longRoot.View());
        const auto filed = initialized ? layout.TryMakeFilePath(path.Value(), file)
                                       : Result<void>::Failure(ErrorCode::OutOfMemory);
        auto logical = layout.TryMakeLogicalPath("Some/Deep/Path/Name.rasset");
        LogicalPathFailure longFailure;
        auto longLogical = layout.TryMakeLogicalPath(longRelative.View(), &longFailure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(initialized && filed && logical);
            CHECK(file.View().EndsWith("/Content/Some/Deep/Path/Name.rasset"));
            CHECK(file.View().StartsWith(longRoot.View()));
            CHECK(logical.Value().Text() == "/Game/Some/Deep/Path/Name");
            CHECK(!longLogical && longFailure.error == LogicalPathError::TooLong);
            break;
        }

        ++failures;
        if (!initialized) {
            CHECK(initialized.Error() == ErrorCode::OutOfMemory);
            CHECK(layout.Root() == "Old");
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

} // namespace project_layout_tests_file
