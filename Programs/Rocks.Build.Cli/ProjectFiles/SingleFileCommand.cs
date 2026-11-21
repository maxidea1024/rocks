namespace Rocks.Build.Cli;

/// <summary>
/// <c>build --files</c>·<c>game --files</c>의 CLI 쪽이다(Docs/Design/ProjectFiles.ko.md 7.4절). 메타 생성까지 평소처럼 한 뒤 호출하고,
/// 고른 소스의 논리 명령을 <see cref="SingleFileCompile"/>로 돌린다. Visual Studio Ctrl+F7이 <c>IdeBuild.ps1 -Action Compile</c>로 부른다.
/// </summary>
internal static class SingleFileCommand
{
    /// <summary>옵션 이름이다(값은 <c>;</c>로 나눈 소스 경로).</summary>
    public const string Option = "files";

    /// <summary>경로 목록을 읽는다. 빈 항목은 건너뛰고, 상대 경로는 기준 디렉터리(엔진 <c>--root</c>, 게임 <c>--project</c>)로 푼다.</summary>
    internal static string[] Parse(string list, string root) =>
        [.. list.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(path => Path.GetFullPath(path, root))];

    /// <summary>파일 하나 컴파일을 돌리고 컴파일러 출력을 낸다. 모두 성공하면 0, 하나라도 실패하면 1이다.</summary>
    public static async Task<int> RunAsync(NativeBuildRequest request, string list, string root, CancellationToken cancellationToken)
    {
        IReadOnlyList<SingleFileResult> results = await SingleFileCompile.RunAsync(request, Parse(list, root), cancellationToken);
        foreach (SingleFileResult result in results)
        {
            Console.Write(result.Process.StdOut);
            Console.Error.Write(result.Process.StdErr);
            Console.WriteLine(result.Process.ExitCode == 0
                ? $"Compiled {result.Source} ({result.Module}) to {result.Object}."
                : $"error: compiling {result.Source} ({result.Module}) failed with exit code {result.Process.ExitCode}.");
        }

        return results.All(result => result.Process.ExitCode == 0) ? 0 : 1;
    }
}
