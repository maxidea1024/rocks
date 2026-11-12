using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class ProgramsPolicyReviewTests
{
    public static void Register(TestSuite suite)
    {
        suite.Add("Policy.ProgramsRuntimeWarningsAndFormattingDiscovery", () =>
        {
            using TemporaryDirectory temporary = new();
            const string path = "Programs/Sample/Private/Main.cpp";
            temporary.Write(path, "int main() { return 0; }\n");
            Verify.That(PolicyChecks.NativeSources(temporary.Root).Any(source =>
                Path.GetRelativePath(temporary.Root, source).Replace('\\', '/') == path),
                "formatter source enumeration skipped Programs");
            Verify.Equal(0, PolicyChecks.Runtime(temporary.Root).Count, "clean program rejected");
            Verify.Equal(0, PolicyChecks.SourceWarnings(temporary.Root).Count, "clean program warning rejected");
            temporary.Write(path, "#include <vector>\n");
            Verify.Equal(1, PolicyChecks.Runtime(temporary.Root).Count, "program STL escaped policy");
            temporary.Write(path, "#pragma clang diagnostic ignored \"-Wunused-variable\"\n");
            Verify.Equal(1, PolicyChecks.SourceWarnings(temporary.Root).Count, "program warning suppression escaped policy");
        });
        suite.Add("Policy.ProgramsNativeBackendBoundaries", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Programs/Commandlet/Private/Win64/Main.cpp", "#include <Windows.h>\n");
            Verify.Equal(0, PolicyChecks.PlatformBoundary(temporary.Root).Count, "native Win64 entry rejected");
            foreach (string path in new[] {
                "Programs/Commandlet/Private/Win64Wrong/Main.cpp",
                "Programs/Commandlet/Private/Mac/Main.mm",
                "Programs/Commandlet/Private/Common/Main.cpp",
                "Programs/CommandletWrong/Private/Win64/Main.cpp",
                "Programs/Other/Private/Win64/Main.cpp",
                // 테스트 소스는 STL 정책 밖이지만 플랫폼 경계는 그대로 적용된다.
                "Core/Source/Runtime/Core/Tests/Private/Win64/Main.cpp" })
            {
                temporary.Write(path, "#include <Windows.h>\n");
                var failures = PolicyChecks.PlatformBoundary(temporary.Root);
                Verify.Equal(1, failures.Count, "lookalike or non-backend platform dependency accepted: " + path);
                Verify.Equal(path, failures[0].Path, "wrong platform violation path");
                File.Delete(temporary.FilePath(path));
            }
        });
        suite.Add("Evidence.ProgramsChangesInvalidateSnapshot", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Programs/Commandlet/Private/Common/Main.cpp", "before\n");
            var before = SourceSnapshot.Capture(temporary.Root);
            temporary.Write("Programs/Commandlet/Private/Common/Main.cpp", "after\n");
            var after = SourceSnapshot.Capture(temporary.Root);
            Verify.That(before != after, "program source mutation escaped snapshot");
            File.Delete(temporary.FilePath("Programs/Commandlet/Private/Common/Main.cpp"));
            Verify.That(after != SourceSnapshot.Capture(temporary.Root), "program source removal escaped snapshot");
        });
    }
}
