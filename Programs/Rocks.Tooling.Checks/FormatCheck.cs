using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 저장소 소스의 서식 검사다. C++(.h·.c·.cpp·.mm)은 고정 버전 clang-format이 루트의 .clang-format으로, C#은 .NET SDK의
/// dotnet format이 .editorconfig의 [*.cs] 규칙(Allman 중괄호·한 줄 한 문장 같은 공백 서식과 IDE0011·IDE2000–IDE2003)으로
/// 검사한다. 서식만 보며 이름·주석·설계 품질은 리뷰가 맡는다.
/// </summary>
public static class FormatCheck
{
    public const string RequiredVersion = "21.1.6";

    /// <summary>
    /// C# 서식 검사 작업 영역(저장소 상대 경로)이다. 저장소의 C# 프로젝트를 모두 담은 솔루션이며 dotnet format을 이 솔루션으로
    /// 한 번만 실행한다(프로젝트 하나를 주면 dotnet format은 참조 프로젝트를 건너뛰고, 프로젝트마다 따로 돌리면 작업 영역을
    /// 프로젝트 수만큼 다시 읽는다).
    /// </summary>
    public const string CSharpWorkspace = "Build/Format/Rocks.Format.slnx";

    // 저장소가 소유한 C# 소스를 찾는 최상위 디렉터리다. bin·obj와 외부 라이브러리 원본은 C++ 검사와 같은 기준으로 뺀다.
    private static readonly string[] CSharpRoots = ["Build", "Core", "Engine", "Programs", "Templates"];

    // dotnet format의 진단 한 줄: `<경로>(<행>,<열>): <심각도> <규칙>: <내용> [<프로젝트>]`. 내용은 SDK UI 언어로 번역되지만
    // 경로·위치·규칙 ID는 번역되지 않는다.
    private static readonly Regex Diagnostic = new(
        @"^(?<path>.+?)\((?<line>\d+),(?<column>\d+)\): (?<severity>\w+) (?<rule>[A-Za-z0-9_]+): (?<message>.*?)(?: \[[^\]]+\])?$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// C++과 C# 서식을 모두 검사한다. apply=false이면 소스를 바꾸지 않고, 위반을 한 줄에 하나씩 모아 InvalidOperationException으로
    /// 알린다(C#은 `경로:행: 규칙: 내용 (column 열)`, C++은 clang-format 출력 그대로). apply=true이면 두 포매터로 고친 뒤, C#은
    /// 자동으로 고칠 수 없는 위반이 남았는지 다시 검사해 남은 것만 알린다.
    /// </summary>
    /// <param name="root">저장소 루트.</param>
    /// <param name="formatter">clang-format 21.1.6 실행 파일(버전이 다르면 InvalidOperationException).</param>
    /// <param name="dotnet">dotnet format·dotnet msbuild를 실행할 .NET SDK 호스트(보통 "dotnet").</param>
    /// <param name="apply">true이면 소스를 고친다.</param>
    /// <param name="environment">자식 프로세스 환경(null이면 이 프로세스의 환경을 물려준다).</param>
    /// <param name="cancellationToken">취소 토큰.</param>
    public static async Task CheckAsync(string root, string formatter, string dotnet, bool apply = false,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        List<string> failures = [.. await CheckNativeAsync(root, formatter, apply, environment, cancellationToken)];
        IReadOnlyList<CheckFailure> csharp = await CheckCSharpAsync(root, dotnet, apply, environment, cancellationToken);
        failures.AddRange(csharp.Select(failure => failure.ToString()));
        if (failures.Count != 0)
        {
            throw new InvalidOperationException(string.Join('\n', failures));
        }
    }

    /// <summary>
    /// 고정 버전 clang-format으로 저장소의 C++ 소스(<see cref="PolicyChecks.NativeSources"/>)를 검증한다. 위반이 있는 묶음마다
    /// clang-format 출력을 하나씩 돌려준다. apply=true이면 고치고(-i), apply=false이면 입력 소스를 변경하지 않는다.
    /// </summary>
    public static async Task<IReadOnlyList<string>> CheckNativeAsync(string root, string formatter, bool apply = false,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        var version = await ProcessRunner.RunAsync(new(formatter, ["--version"], root, environment,
            Timeout: TimeSpan.FromSeconds(15)), cancellationToken);
        if (version.ExitCode != 0 || !Regex.IsMatch(version.StdOut,
            @"version\s+21\.1\.6(?:\s|$)", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)))
        {
            throw new InvalidOperationException("Rocks requires clang-format " + RequiredVersion);
        }

        List<string> failures = [];
        IReadOnlyList<string> files = PolicyChecks.NativeSources(root);
        for (int offset = 0; offset < files.Count; offset += 40)
        {
            List<string> arguments = apply ? ["-i"] : ["--dry-run", "--Werror"];
            arguments.AddRange(files.Skip(offset).Take(40));
            var result = await ProcessRunner.RunAsync(new(formatter, arguments, root, environment,
                Timeout: TimeSpan.FromSeconds(60)), cancellationToken);
            if (result.ExitCode != 0)
            {
                failures.Add("clang-format failed:\n" + result.StdOut + result.StdErr);
            }
        }

        return failures;
    }

    /// <summary>
    /// 저장소의 C# 소스 서식을 검사한다. 두 가지를 함께 본다.
    /// <list type="bullet">
    /// <item>소속: 저장소의 C# 소스(Build·Core·Engine·Programs·Templates, bin·obj·외부 라이브러리 원본 제외)가 모두
    /// <see cref="CSharpWorkspace"/> 프로젝트의 Compile 항목인가(<see cref="CheckCSharpProjectsAsync"/>). dotnet format은 프로젝트에
    /// 속한 파일만 보므로, 빠진 파일은 규칙 <c>csharp-project</c>로 알린다.</item>
    /// <item>서식: <c>dotnet format &lt;작업 영역&gt; --verify-no-changes --severity warn</c>(공백 서식·코드 스타일·분석기). 위반은
    /// 파일·행·규칙 ID(공백 서식은 <c>WHITESPACE</c>, 스타일은 IDE0011 같은 ID)로 돌려준다. 작업 영역을 읽다가 낸 경고도
    /// 규칙 <c>dotnet-format</c>으로 알린다 — 일부 프로젝트를 읽지 못하고 지나갔을 수 있기 때문이다.</item>
    /// </list>
    /// apply=true이면 먼저 <c>dotnet format &lt;작업 영역&gt; --severity warn</c>으로 고치고, 남은 위반을 같은 방식으로 다시 검사한다.
    /// 위반이 없으면 빈 목록이다. 작업 영역 파일이 없으면 FileNotFoundException, dotnet을 실행할 수 없거나 제한 시간을 넘기면
    /// 예외를 그대로 전파한다.
    /// </summary>
    public static async Task<IReadOnlyList<CheckFailure>> CheckCSharpAsync(string root, string dotnet, bool apply = false,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        string workspace = Path.Combine(root, CSharpWorkspace);
        if (!File.Exists(workspace))
        {
            throw new FileNotFoundException("C# format workspace not found", workspace);
        }

        // 소속 확인(프로젝트 평가, 1초 안팎)은 dotnet format보다 먼저 끝낸다. dotnet format의 복원이 아직 복원하지 않은 프로젝트의
        // obj 파일(nuget.g.props 등)을 쓰는 동안 같은 프로젝트를 평가하면 쓰는 중인 파일을 읽을 수 있다.
        List<CheckFailure> failures = [.. await CheckCSharpProjectsAsync(root, dotnet, environment, cancellationToken)];
        if (apply)
        {
            var fixedResult = await ProcessRunner.RunAsync(new(dotnet, ["format", workspace, "--severity", "warn"], root, environment,
                Timeout: TimeSpan.FromMinutes(10)), cancellationToken);
            if (fixedResult.ExitCode != 0)
            {
                failures.Add(new(CSharpWorkspace, 0, "dotnet-format",
                    $"dotnet format exited with {fixedResult.ExitCode}:\n{fixedResult.StdOut}{fixedResult.StdErr}"));
            }
        }

        var verified = await ProcessRunner.RunAsync(new(dotnet, ["format", workspace, "--verify-no-changes", "--severity", "warn"],
            root, environment, Timeout: TimeSpan.FromMinutes(10)), cancellationToken);
        failures.AddRange(FormatFailures(root, verified));
        return failures;
    }

    /// <summary>
    /// 저장소의 C# 소스가 모두 <see cref="CSharpWorkspace"/>에 든 프로젝트의 Compile 항목인지 확인한다. 프로젝트마다
    /// <c>dotnet msbuild &lt;프로젝트&gt; -getItem:Compile</c>로 평가(빌드 없이)한 결과와 저장소의 .cs 목록을 비교하고, 어느
    /// 프로젝트에도 속하지 않는 파일을 규칙 <c>csharp-project</c>로 돌려준다(새 C# 프로젝트를 솔루션에 넣지 않았거나 규칙 파일이
    /// 규칙 프로젝트의 glob 밖에 있는 경우). 작업 영역 파일이 없으면 FileNotFoundException, 프로젝트 평가가 실패하면
    /// InvalidOperationException이다.
    /// </summary>
    public static async Task<IReadOnlyList<CheckFailure>> CheckCSharpProjectsAsync(string root, string dotnet,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        string workspace = Path.Combine(root, CSharpWorkspace);
        if (!File.Exists(workspace))
        {
            throw new FileNotFoundException("C# format workspace not found", workspace);
        }

        StringComparer comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        string directory = Path.GetDirectoryName(workspace)!;
        string[] projects = XDocument.Load(workspace).Descendants("Project")
            .Select(project => (string?)project.Attribute("Path") ?? throw new InvalidDataException(CSharpWorkspace + ": Project without Path"))
            .Select(path => Path.GetFullPath(Path.Combine(directory, path.Replace('\\', '/'))))
            .ToArray();
        IReadOnlyList<string>[] items = await Task.WhenAll(projects.Select(project =>
            CompileItemsAsync(root, dotnet, project, environment, cancellationToken)));
        HashSet<string> compiled = new(items.SelectMany(paths => paths), comparer);
        IReadOnlyList<string> upstream = PolicyChecks.UpstreamDirectories(root);
        return PolicyChecks.Files(root, CSharpRoots)
            .Where(path => Path.GetExtension(path).Equals(".cs", StringComparison.OrdinalIgnoreCase))
            .Select(path => (Path: path, Relative: Relative(root, path)))
            .Where(source => !upstream.Any(prefix => source.Relative.StartsWith(prefix, StringComparison.Ordinal)))
            .Where(source => !compiled.Contains(source.Path))
            .Select(source => new CheckFailure(source.Relative, 0, "csharp-project",
                $"C# source is not compiled by any project in {CSharpWorkspace}, so dotnet format does not check it; " +
                "add it to a project there"))
            .ToArray();
    }

    // 빌드 없이 평가만 한 프로젝트의 Compile 항목(절대 경로)이다. 생성 파일(obj의 AssemblyInfo 등)은 빌드 단계에서 더해지므로
    // 들어오지 않는다.
    private static async Task<IReadOnlyList<string>> CompileItemsAsync(string root, string dotnet, string project,
        IReadOnlyDictionary<string, string?>? environment, CancellationToken cancellationToken)
    {
        var result = await ProcessRunner.RunAsync(new(dotnet, ["msbuild", project, "-nologo", "-getItem:Compile"], root, environment,
            Timeout: TimeSpan.FromMinutes(2)), cancellationToken);
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException($"MSBuild evaluation failed for {Relative(root, project)}:\n{result.StdOut}{result.StdErr}");
        }

        using JsonDocument document = JsonDocument.Parse(result.StdOut);
        if (!document.RootElement.TryGetProperty("Items", out JsonElement itemGroups) ||
            !itemGroups.TryGetProperty("Compile", out JsonElement compile))
        {
            return [];
        }

        return compile.EnumerateArray().Select(item => Path.GetFullPath(item.GetProperty("FullPath").GetString()!)).ToArray();
    }

    // dotnet format 검증 결과를 위반 목록으로 바꾼다. 종료 코드 0은 통과, 2는 서식 차이(진단이 표준 오류에 한 줄씩)다. 진단이 아닌
    // 표준 오류 줄(작업 영역 경고 등)과 그 밖의 종료 코드는 출력 그대로 실패로 알린다.
    private static IReadOnlyList<CheckFailure> FormatFailures(string root, ProcessResult result)
    {
        List<CheckFailure> failures = [];
        HashSet<string> seen = new(StringComparer.Ordinal);
        StringBuilder unexpected = new();
        foreach (string line in result.StdErr.Split('\n').Select(text => text.TrimEnd('\r')).Where(text => text.Length != 0))
        {
            Match match = Diagnostic.Match(line);
            if (!match.Success)
            {
                unexpected.Append(line).Append('\n');
                continue;
            }

            string path = Relative(root, match.Groups["path"].Value);
            int lineNumber = int.Parse(match.Groups["line"].Value, System.Globalization.CultureInfo.InvariantCulture);
            string rule = match.Groups["rule"].Value;
            string message = $"{match.Groups["message"].Value} (column {match.Groups["column"].Value})";
            // 한 파일이 여러 프로젝트에 들어 있으면 같은 진단이 프로젝트마다 나온다.
            if (seen.Add($"{path}\n{lineNumber}\n{rule}\n{message}"))
            {
                failures.Add(new(path, lineNumber, rule, message));
            }
        }

        List<CheckFailure> sorted = [.. failures.OrderBy(failure => failure.Path, StringComparer.Ordinal).ThenBy(failure => failure.Line)];
        if (result.ExitCode is not (0 or 2) || unexpected.Length != 0 || (result.ExitCode == 2 && sorted.Count == 0))
        {
            sorted.Add(new(CSharpWorkspace, 0, "dotnet-format",
                $"dotnet format exited with {result.ExitCode} and printed more than formatting diagnostics (a workspace warning can " +
                $"mean a skipped project); rerun `dotnet format {CSharpWorkspace} --verify-no-changes --severity warn -v diag` " +
                $"for details:\n{unexpected}{result.StdOut}"));
        }

        return sorted;
    }

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
}
