using Rocks.Build;
using Rocks.Build.Cli;

namespace Rocks.Build.IntegrationTests;

/// <summary>
/// <c>unity-profile</c> 명령이 잘못된 입력을 빌드 전에 거부하는지 본다(Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측"): 모르는
/// 옵션, 빠진 <c>--module</c>, 잘못된 조합 목록, 이 명령의 것이 아닌 출력 디렉터리, 모르는 모듈. 어느 경우도 컴파일까지 가지 않는다.
/// 명령 전체(모듈을 조합마다 다시 컴파일)는 비용 때문에 여기서 돌리지 않는다.
/// </summary>
internal static class UnityProfileCommandTests
{
    // 저장소에 없는 모듈 이름이다.
    private const string MissingModule = "NoSuchModuleForUnityProfile";

    /// <summary>
    /// <paramref name="root"/>는 저장소 루트(규칙 어셈블리가 빌드되어 있어야 한다), <paramref name="temporary"/>는 빈 임시 디렉터리,
    /// <paramref name="compiler"/>는 시험 실행이 받은 컴파일러 경로다(없으면 기본 탐색).
    /// </summary>
    internal static async Task RunAsync(string root, string temporary, string? compiler = null)
    {
        // 옵션 검사: 저장소도 툴체인도 보지 않고 거부한다.
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--timing", "on"], "is not supported by 'unity-profile'");
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--no-such-option", "1"], "Unknown, duplicate, or incomplete option");
        await RejectsAsync(["unity-profile", "--root", temporary], "requires --module");
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", " "], "requires --module");
        foreach (string list in new[] { "0", "8,,16", "8,", "-8", "eight", "" })
        {
            await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--anchor-files", list], "comma-separated list of positive integers");
            await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--batch-kib", list], "comma-separated list of positive integers");
        }

        // build 명령은 이 명령의 옵션을 받지 않는다.
        await RejectsAsync(["build", "--root", temporary, "--module", "Core"], "is not supported by 'build'");

        // 출력 디렉터리: <root>/out 밖, 접두로 시작하지 않는 이름(평소 빌드 출력), 표지 없이 증분 기록이 있는 디렉터리를 거부한다. 거부한
        // 출력에는 아무것도 쓰지 않는다.
        string outputs = Directory.CreateDirectory(Path.Combine(temporary, "out")).FullName;
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--output", Path.Combine(temporary, "elsewhere", "unity-profile-x")],
            "--output must be under");
        string usual = Directory.CreateDirectory(Path.Combine(outputs, "dotnet-windows-foundation-debug")).FullName;
        File.WriteAllText(Path.Combine(usual, "native-cache.json"), "{}");
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--output", usual], "must start with 'unity-profile-'");
        string foreign = Directory.CreateDirectory(Path.Combine(outputs, "unity-profile-foreign")).FullName;
        File.WriteAllText(Path.Combine(foreign, "native-cache.json"), "{}");
        await RejectsAsync(["unity-profile", "--root", temporary, "--module", "Core", "--output", foreign], "holds the outputs of another build");
        foreach (string untouched in new[] { usual, foreign })
        {
            Require(Directory.GetFileSystemEntries(untouched).Select(Path.GetFileName).SequenceEqual(["native-cache.json"]) &&
                File.ReadAllText(Path.Combine(untouched, "native-cache.json")) == "{}", "A rejected output directory was modified: " + untouched);
        }

        // 모르는 모듈: 규칙을 읽은 직후, 출력에 보고서·잠금을 만들기 전에 거부한다(표지만 남는다).
        string claimed = Path.Combine(root, "out", UnityProfile.OutputPrefix + "command-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            string[] command = ["unity-profile", "--root", root, "--module", MissingModule, "--output", claimed, "--anchor-files", "16", "--batch-kib", "384"];
            await RejectsAsync(compiler is null ? command : [.. command, "--compiler", compiler], "Unknown module " + MissingModule);
            Require(Directory.GetFileSystemEntries(claimed).Select(Path.GetFileName).SequenceEqual([UnityProfile.MarkerFileName]),
                "An unknown module must be rejected before the build writes anything: " + string.Join(", ", Directory.GetFileSystemEntries(claimed)));
        }
        finally
        {
            if (Directory.Exists(claimed))
            {
                Directory.Delete(claimed, recursive: true);
            }
        }
    }

    private static async Task RejectsAsync(string[] arguments, string expected)
    {
        try
        {
            int exit = await BuildCommand.RunAsync(arguments, CancellationToken.None);
            throw new InvalidOperationException($"'{string.Join(' ', arguments)}' was accepted (exit {exit}); expected: {expected}");
        }
        catch (ArgumentException error) when (error.Message.Contains(expected, StringComparison.Ordinal))
        {
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
