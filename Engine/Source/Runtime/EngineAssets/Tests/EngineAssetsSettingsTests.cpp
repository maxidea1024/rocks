#include <Core/Memory/SystemAllocator.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// EngineAssets 설정 결정 시험이다(Docs/Design/AssetSystem 11.8절 3, 10.3.1절 A4f4 5번): 쿠킹
// 루트(설정 값 → 엔진 전용 실행의 엔진 쿠킹 루트 → 실행 파일 디렉터리 + /Cooked), 실행 프로파일
// 기본값, 실행 파일 경로의 디렉터리 부분.
using namespace rocks;
namespace engine_assets_settings_tests_file {

// 쿠킹 루트: 설정 값이 있으면 그대로(엔진 디렉터리·실행 파일 디렉터리와 무관), 없고 엔진 전용
// 디렉터리가 있으면 `<엔진>/Saved/Cooked/<프로파일>`(프로파일이 비면 InvalidArgument), 그것도
// 없으면 실행 파일 디렉터리 + "/Cooked", 모두 없으면 InvalidArgument, out 뒤에 붙이며 할당 실패는
// 그 오류다.
TEST(EngineAssets_ResolveCookedRoot) {
    SystemAllocator allocator;
    {
        String out(allocator);
        REQUIRE(ResolveCookedRoot("D:/Game/Saved/Cooked/Windows", "E:/Engine", "Windows", "C:/Bin",
                                  out));
        CHECK(out.View() == "D:/Game/Saved/Cooked/Windows");
    }

    {
        String out(allocator);
        REQUIRE(ResolveCookedRoot("", "", "Windows", "C:/Bin", out));
        CHECK(out.View() == "C:/Bin/Cooked");
    }

    {
        String out(allocator);
        REQUIRE(ResolveCookedRoot("", "E:/Rocks/Engine", "Mac", "C:/Bin", out));
        CHECK(out.View() == "E:/Rocks/Engine/Saved/Cooked/Mac");
        String noProfile(allocator);
        CHECK(ResolveCookedRoot("", "E:/Rocks/Engine", "", "C:/Bin", noProfile).Error() ==
              ErrorCode::InvalidArgument);
    }

    {
        String out(allocator);
        REQUIRE(ResolveCookedRoot("Relative/Cooked", "", "", "", out));
        CHECK(out.View() == "Relative/Cooked");
    }

    {
        String out(allocator);
        CHECK(ResolveCookedRoot("", "", "Windows", "", out).Error() == ErrorCode::InvalidArgument);
        CHECK(out.View().IsEmpty());
    }

    test::NFailAllocator failing(allocator);
    for (uint64 attempt = 1; attempt <= 2; ++attempt) {
        String out(failing);
        failing.FailOnAttempt(attempt);
        const auto resolved =
            ResolveCookedRoot("", "", "Windows", "C:/A/Fairly/Long/Binary/Directory/Path", out);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (failed) {
            CHECK(resolved.Error() == ErrorCode::OutOfMemory);
        } else {
            CHECK(resolved.IsOk());
        }
    }
}

// 실행 프로파일: 설정 값이 있으면 그대로, 없으면 빌드 플랫폼 이름이다.
TEST(EngineAssets_ResolveRuntimeProfile) {
    CHECK(ResolveRuntimeProfile("Custom") == "Custom");
    const StringView fallback = ResolveRuntimeProfile("");
    if constexpr (IsWindows) {
        CHECK(fallback == "Windows");
    } else if constexpr (IsMac) {
        CHECK(fallback == "Mac");
    } else {
        CHECK(fallback == "Android");
    }
}

// 실행 파일 디렉터리: 마지막 '/'·'\\' 앞이고 구분자는 빼며, 루트 하나면 그 구분자, 구분자가 없으면
// 빈 값이다.
TEST(EngineAssets_ExecutableDirectoryOf) {
    CHECK(ExecutableDirectoryOf("C:/Game/Binaries/Game.exe") == "C:/Game/Binaries");
    CHECK(ExecutableDirectoryOf("C:\\Game\\Binaries\\Game.exe") == "C:\\Game\\Binaries");
    CHECK(ExecutableDirectoryOf("C:/Game/Mixed\\Game.exe") == "C:/Game/Mixed");
    CHECK(ExecutableDirectoryOf("/game") == "/");
    CHECK(ExecutableDirectoryOf("Game.exe").IsEmpty());
    CHECK(ExecutableDirectoryOf("").IsEmpty());
}

} // namespace engine_assets_settings_tests_file
