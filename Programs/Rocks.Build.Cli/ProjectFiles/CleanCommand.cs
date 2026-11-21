using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// <c>clean</c> 명령이다(Docs/Design/ProjectFiles.ko.md 7.3절). 엔진 타깃·구성·플랫폼의 출력 디렉터리 하나를 비운다. Visual Studio의
/// Clean·Rebuild가 부른다(Rebuild는 clean 다음 build).
/// 사용 예: <c>Rocks.Build.Cli clean --root . --target Sandbox --configuration Debug</c>.
/// <list type="bullet">
/// <item><c>--platform host|android</c>(기본 host), <c>--android-abi</c>(기본 arm64), <c>--output</c>: build와 같은 출력 경로 규칙이다.</item>
/// <item>지울 디렉터리는 <c>&lt;root&gt;/out</c>의 엄격한 하위이고 Rocks 출력 표지(<c>.native-build.lock</c> 또는 <c>.rocks-invocation.lock</c>)가 있어야
/// 한다. 아니면 거부한다. 호출 잠금만 있는 출력은 네이티브 빌드 전에 실패한 빌드(규칙·메타 단계)의 것이다.
/// 출력이 없으면 할 일이 없는 성공이다.</item>
/// <item>그 출력의 호출 잠금을 잡은 뒤 잠금 파일 둘(<c>.rocks-invocation.lock</c>·<c>.native-build.lock</c>)을 뺀 모든 항목을 지운다.</item>
/// </list>
/// 게임 프로젝트는 <c>clean --project &lt;게임 디렉터리&gt; --target &lt;T&gt; --configuration &lt;C&gt;</c>이다(출력 <c>&lt;project&gt;/out/&lt;platform&gt;-&lt;target&gt;-&lt;configuration&gt;</c>,
/// 경계 루트는 프로젝트 디렉터리).
/// </summary>
internal static class CleanCommand
{
    public const string Name = "clean";

    /// <summary>Rocks 빌드 출력 디렉터리의 표지 파일이다(빌드 엔진의 출력 잠금 파일).</summary>
    public const string OutputMarker = ".native-build.lock";

    // 지우지 않는 잠금 파일이다(호출 잠금은 이 명령이 쥐고 있고, 빌드 잠금은 출력 표지다).
    // 출력 호출 잠금 파일이다(OutputInvocationLock — 네이티브 빌드 전에 실패한 출력에도 있다).
    private const string InvocationMarker = ".rocks-invocation.lock";

    private static readonly string[] KeptFiles = [InvocationMarker, OutputMarker];

    public static async Task<int> RunAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        string target = options.TryGetValue("target", out string? name) && name.Length != 0
            ? name : throw new ArgumentException($"Command '{Name}' requires --target <name>.");
        NativeConfiguration configuration = BuildCommand.Configuration(options);
        // 게임 프로젝트(--project): 게임 출력 규칙(GameProjectLayout)이고 경계 루트는 프로젝트 디렉터리다(Docs/Design/ProjectFiles.ko.md 7.3절).
        if (options.TryGetValue("project", out string? project))
        {
            if (options.ContainsKey("root") || options.ContainsKey("platform") || options.ContainsKey("android-abi"))
            {
                throw new ArgumentException("--project cleans a game output; --root, --platform and --android-abi belong to engine targets.");
            }

            project = PathSpelling.OnDisk(Path.GetFullPath(project));
            string gameOutput = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(options.GetValueOrDefault("output",
                GameProjectLayout.DefaultOutput(project, target, configuration)))));
            int cleaned = await CleanAsync(project, gameOutput, cancellationToken);
            Console.WriteLine(cleaned < 0 ? $"Clean: {gameOutput} does not exist; nothing to clean." : $"Clean: removed {cleaned} entries from {gameOutput}.");
            return 0;
        }

        string root = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory())));
        string platform = options.GetValueOrDefault("platform", "host");
        if (platform is not ("host" or "android"))
        {
            throw new ArgumentException("platform must be host or android.");
        }

        string platformName = platform == "android" ? "android-" + options.GetValueOrDefault("android-abi", "arm64")
            : OperatingSystem.IsWindows() ? "windows" : "macos";
        string output = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(options.GetValueOrDefault("output",
            EnginePlanning.DefaultOutput(root, platformName, target, configuration)))));
        int removed = await CleanAsync(root, output, cancellationToken);
        Console.WriteLine(removed < 0 ? $"Clean: {output} does not exist; nothing to clean." : $"Clean: removed {removed} entries from {output}.");
        return 0;
    }

    /// <summary>
    /// 출력 디렉터리 하나를 비운다. 지운 항목 수를 돌려주고, 출력이 없으면 -1이다.
    /// </summary>
    /// <exception cref="ArgumentException"><c>&lt;root&gt;/out</c>의 엄격한 하위가 아니거나 Rocks 출력 표지가 없다.</exception>
    public static async Task<int> CleanAsync(string root, string output, CancellationToken cancellationToken)
    {
        if (!DirectoryPath.IsStrictlyUnder(output, Path.Combine(root, "out")))
        {
            throw new ArgumentException($"Clean refuses {output}: only build outputs under {Path.Combine(root, "out")} are cleaned.");
        }

        if (!Directory.Exists(output))
        {
            return -1;
        }

        if (!File.Exists(Path.Combine(output, OutputMarker)) && !File.Exists(Path.Combine(output, InvocationMarker)))
        {
            throw new ArgumentException($"Clean refuses {output}: it has no {OutputMarker} or {InvocationMarker}, so it is not a Rocks build output.");
        }

        await using var invocationLock = await OutputInvocationLock.AcquireAsync(output, cancellationToken);
        int removed = 0;
        foreach (string entry in Directory.EnumerateFileSystemEntries(output).Order(StringComparer.Ordinal).ToArray())
        {
            if (KeptFiles.Contains(Path.GetFileName(entry), StringComparer.OrdinalIgnoreCase))
            {
                continue;
            }

            if (Directory.Exists(entry))
            {
                Directory.Delete(entry, recursive: true);
            }
            else
            {
                File.Delete(entry);
            }

            ++removed;
        }

        return removed;
    }
}
