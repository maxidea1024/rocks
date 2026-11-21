namespace Rocks.Build.Cli;

/// <summary>
/// 게임 프로젝트 디렉터리의 배치 규칙이다(Docs/Design/ProjectFiles.ko.md 10.5·10.6절).
/// <list type="bullet">
/// <item>기본 출력: <c>&lt;project&gt;/out/&lt;platform&gt;-&lt;target&gt;-&lt;configuration&gt;</c>(소문자) — 타깃이 둘 이상이어도 겹치지 않고 clean이 지울
/// 곳이 하나로 정해진다.</item>
/// <item>입력에서 빼는 곳: 깊이와 무관한 점 디렉터리(<c>.git</c>·<c>.vs</c>·<c>.vscode</c>·<c>.idea</c>), 최상위 <c>out/</c>·<c>Saved/</c>,
/// 최상위 <c>*.slnx</c>(세 이름은 대소문자 무관). 게임 입력 snapshot과 규칙 파일 탐색이 같은 규칙을 쓴다.</item>
/// </list>
/// </summary>
internal static class GameProjectLayout
{
    // 프로젝트 루트 바로 아래에서 입력이 아닌 디렉터리다(빌드·IDE 출력, 실행 중 크래시 수집).
    private static readonly string[] ExcludedTopDirectories = ["out", "Saved"];

    // 프로젝트 루트 바로 아래에서 입력이 아닌 파일의 확장자다(생성 솔루션).
    private const string SolutionExtension = ".slnx";

    // out·Saved·.slnx는 호스트와 무관하게 대소문자를 가리지 않는다(macOS APFS 기본 볼륨도 가리지 않아 saved/·OUT/가 같은 디렉터리다).
    private const StringComparison NameComparison = StringComparison.OrdinalIgnoreCase;

    /// <summary>호스트의 게임 출력 플랫폼 이름이다(엔진 출력 이름과 같은 철자).</summary>
    public static string HostPlatformName => OperatingSystem.IsWindows() ? "windows" : "macos";

    /// <summary>
    /// <c>--target</c>가 없을 때의 게임 타깃 이름이다: 게임 루트의 <c>.rocksproject</c>(정확히 하나) 이름이다. UE도 <c>&lt;프로젝트&gt;.Target.cs</c>가 기본
    /// 게임 타깃이고, 템플릿에서 만든 프로젝트의 타깃 이름이 프로젝트 이름이다(Docs/Design/ProjectFiles.ko.md 11.1절).
    /// </summary>
    public static string DefaultTarget(string project)
    {
        string[] files = Directory.GetFiles(project, "*" + Rocks.Build.Descriptors.DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
        return files.Length == 1 ? Path.GetFileNameWithoutExtension(files[0])
            : throw new ArgumentException($"The game project root must contain exactly one {Rocks.Build.Descriptors.DescriptorReader.ProjectExtension} file " +
                $"(found {files.Length}); pass --target <name>.");
    }

    /// <summary>게임 타깃·구성의 기본 출력 디렉터리다.</summary>
    public static string DefaultOutput(string project, string target, NativeConfiguration configuration) =>
        Path.Combine(project, "out", $"{HostPlatformName}-{target.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}");

    /// <summary>
    /// 프로젝트 아래 경로가 게임 입력에서 빠지는가. 프로젝트 밖 경로는 빠지지 않는다(명시 입력은 이 규칙과 무관하게 호출자가 본다).
    /// </summary>
    public static bool IsExcluded(string project, string path)
    {
        string relative = Path.GetRelativePath(Path.GetFullPath(project), Path.GetFullPath(path));
        if (relative == "." || Path.IsPathRooted(relative) || relative == ".." || relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
        {
            return false;
        }

        string[] segments = relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        bool directory = Directory.Exists(path);
        // 점 디렉터리는 깊이와 무관하게 뺀다(마지막 성분이 파일이면 파일 이름은 보지 않는다).
        int directories = directory ? segments.Length : segments.Length - 1;
        for (int index = 0; index < directories; ++index)
        {
            if (segments[index].Length > 1 && segments[index][0] == '.')
            {
                return true;
            }
        }

        // 최상위 out/·Saved/ 아래(그 디렉터리 자신 포함)와 최상위 *.slnx 파일이다.
        if ((segments.Length > 1 || directory) && ExcludedTopDirectories.Any(name => string.Equals(segments[0], name, NameComparison)))
        {
            return true;
        }

        return segments.Length == 1 && !directory && segments[0].EndsWith(SolutionExtension, NameComparison);
    }

    /// <summary>
    /// 게임 규칙 파일(<c>*.Build.cs</c>·<c>*.Target.cs</c>)이다. 빼는 곳, 출력, 프로젝트 안에 둔 SDK 아래로는 내려가지 않는다. 링크(symlink·
    /// junction)는 따라가지 않고 거부한다(입력 snapshot과 같다). 정렬해 돌려준다.
    /// </summary>
    public static string[] RuleFiles(string project, string output, string sdk)
    {
        var files = new List<string>();
        var pending = new Stack<string>([Path.GetFullPath(project)]);
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            foreach (string file in Directory.EnumerateFiles(directory, "*.cs"))
            {
                RejectLink(file);
                if ((file.EndsWith(".Build.cs", StringComparison.Ordinal) || file.EndsWith(".Target.cs", StringComparison.Ordinal)) && !IsExcluded(project, file))
                {
                    files.Add(file);
                }
            }

            foreach (string child in Directory.EnumerateDirectories(directory))
            {
                if (!IsExcluded(project, child) && !Tooling.Common.DirectoryPath.IsAtOrUnder(child, output) &&
                    !Tooling.Common.DirectoryPath.IsAtOrUnder(child, sdk))
                {
                    RejectLink(child);
                    pending.Push(child);
                }
            }
        }

        return [.. files.Order(StringComparer.Ordinal)];
    }

    private static void RejectLink(string path)
    {
        if (FileLinks.IsLink(path))
        {
            throw new IOException("Game rule discovery cannot follow links: " + path);
        }
    }
}
