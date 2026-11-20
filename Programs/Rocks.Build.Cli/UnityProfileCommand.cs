using System.Globalization;
using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 실측 실행 하나에 얹는 요청이다. <see cref="BuildCommand"/>의 build 흐름이 이 값을 보고 동작을 바꾼다.
/// <list type="bullet">
/// <item><see cref="Case"/>가 있으면 조합 하나를 잰다: 그 모듈만 재정의·재컴파일하고 끝나면 <see cref="Row"/>를 채운다.</item>
/// <item><see cref="Case"/>가 null이면 준비 실행이다: 그 모듈이 의존하는 모듈만 빌드한다.</item>
/// </list>
/// 흐름은 <see cref="ResolvedModule"/>(규칙의 정식 모듈 이름)을 채우고, native 빌드가 실패하면 <see cref="Failure"/>(첫 오류 한 줄)를 채운다.
/// </summary>
internal sealed class UnityProfileRun(string module, UnityProfileCase? profile)
{
    public string Module { get; } = module;

    public UnityProfileCase? Case { get; } = profile;

    public string? ResolvedModule { get; set; }

    public UnityProfileRow? Row { get; set; }

    public string? Failure { get; set; }
}

/// <summary>
/// <c>unity-profile</c> 명령이다(Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측"). 모듈 하나를 "유니티 끔"과
/// <c>UnityAnchorFiles</c>·묶음 상한 조합마다 처음부터 컴파일하고, 조합별 번역 단위 수·CPU 시간·벽시계 시간·최대 메모리 표를 낸다.
/// 사용 예: <c>Rocks.Build.Cli unity-profile --root . --module Core</c>.
/// <list type="bullet">
/// <item><c>--module</c>(필수): 재는 모듈. <c>--target</c>(기본 Foundation)이 빌드하는 모듈이어야 한다.</item>
/// <item><c>--anchor-files</c>·<c>--batch-kib</c>: 쉼표로 나눈 값 목록. 기본은 8,16,24,32와 256,384,512다.</item>
/// <item><c>--configuration</c>·<c>--compiler</c>·<c>--rules</c>·<c>--jobs</c>: build와 같다.</item>
/// <item><c>--output</c>: 기본은 <c>out/unity-profile-&lt;타깃&gt;-&lt;구성&gt;</c>이다. 폴더 이름이 <c>unity-profile-</c>로 시작해야 하고, 다른
/// 빌드의 출력(표지 파일 없이 증분 기록이 있는 디렉터리)은 거부한다 — 이 명령은 그 모듈의 object를 지우고 보고서를 덮어쓴다.</item>
/// </list>
/// 먼저 그 모듈이 의존하는 모듈만 빌드하는 준비 실행을 하고, 그다음 조합마다 그 모듈의 object·묶음 파일·PCH를 지우고 다시 빌드한다.
/// 조합 하나가 실패해도 나머지를 재고, 표와 <c>&lt;출력&gt;/UnityProfile/&lt;모듈&gt;.unity-profile.json</c>을 쓴 뒤 0이 아닌 값으로 끝난다.
/// </summary>
internal static class UnityProfileCommand
{
    public const string Name = "unity-profile";

    private const string ModuleOption = "module";

    private const string AnchorFilesOption = "anchor-files";

    private const string BatchKibibytesOption = "batch-kib";

    private const string DefaultTarget = "Foundation";

    // 빌드가 예외로 끝난 조합의 종료 코드다(명령줄 도구가 예외에 쓰는 값과 같다).
    private const int ExceptionExitCode = 2;

    // 조합이 하나라도 실패했을 때 명령의 종료 코드다.
    private const int FailedCasesExitCode = 1;

    // 이 명령만 받는 옵션이다(명령줄 파서의 전체 옵션 목록에 더해진다).
    internal static readonly string[] Options = [ModuleOption, AnchorFilesOption, BatchKibibytesOption];

    private static readonly HashSet<string> Allowed =
        new(["root", "target", "configuration", "output", "compiler", "rules", "jobs", .. Options], StringComparer.Ordinal);

    public static async Task<int> RunAsync(string[] arguments, CancellationToken cancellationToken)
    {
        Dictionary<string, string> options = BuildCommand.Parse(arguments);
        foreach (string name in options.Keys)
        {
            if (!Allowed.Contains(name))
            {
                throw new ArgumentException($"Option --{name} is not supported by '{Name}'.");
            }
        }

        if (!options.TryGetValue(ModuleOption, out string? module) || string.IsNullOrWhiteSpace(module))
        {
            throw new ArgumentException($"Command '{Name}' requires --{ModuleOption} <name>.");
        }

        IReadOnlyList<UnityProfileCase> cases = UnityProfile.Cases(
            Values(options, AnchorFilesOption, UnityProfile.DefaultAnchorFiles), Values(options, BatchKibibytesOption, UnityProfile.DefaultBatchKibibytes));
        string root = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory())));
        string target = options.GetValueOrDefault("target", DefaultTarget);
        NativeConfiguration configuration = BuildCommand.Configuration(options);
        string output = Path.GetFullPath(options.GetValueOrDefault("output",
            Path.Combine(root, "out", $"{UnityProfile.OutputPrefix}{target.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}")));
        // 이 명령은 그 모듈의 object를 지우고 compile DB·보고서·증분 기록을 덮어쓴다. 평소 빌드 출력에 대고 돌지 않게 먼저 확인한다
        // (build 흐름도 같은 조건을 요구한다 — 출력은 <root>/out 아래).
        if (!DirectoryPath.IsStrictlyUnder(output, Path.Combine(root, "out")))
        {
            throw new ArgumentException($"--output must be under {Path.Combine(root, "out")}: {output}");
        }

        UnityProfile.ClaimOutput(output);

        // build 명령에 넘기는 인자다. 작업 파일 분리는 끈다(작업 중인 파일에 따라 묶음 구성이 달라지면 조합끼리 비교할 수 없다).
        var build = new List<string> { "build", "--root", root, "--target", target, "--configuration", configuration.ToString(), "--output", output,
            "--adaptive", "off" };
        foreach (string name in new[] { "compiler", "rules", "jobs" })
        {
            if (options.TryGetValue(name, out string? value))
            {
                build.AddRange(["--" + name, value]);
            }
        }

        // 준비 실행: 그 모듈이 의존하는 모듈과 생성 코드를 빌드해 둔다(그 모듈 자신은 컴파일하지 않는다). 실패하면 잰 것이 없으므로 끝낸다.
        Console.WriteLine($"{Name}: preparing dependencies of {module} ({cases.Count} cases follow)");
        var prepare = new UnityProfileRun(module, null);
        int prepared = await BuildCommand.RunAsync([.. build], cancellationToken, prepare);
        if (prepared != 0)
        {
            return prepared;
        }

        // 규칙의 정식 모듈 이름이다(명령줄 철자의 대소문자와 다를 수 있다).
        string moduleName = prepare.ResolvedModule ?? throw new InvalidOperationException("The preparation build did not resolve the module.");
        var rows = new List<UnityProfileRow>();
        foreach (UnityProfileCase profile in cases)
        {
            Console.WriteLine($"{Name}: {moduleName}: {profile.Name}");
            var run = new UnityProfileRun(module, profile);
            try
            {
                int exit = await BuildCommand.RunAsync([.. build], cancellationToken, run);
                rows.Add(exit == 0 && run.Row is not null
                    ? run.Row
                    : UnityProfileRow.Failed(profile, exit == 0 ? FailedCasesExitCode : exit,
                        string.IsNullOrEmpty(run.Failure) ? $"the build exited with code {exit} without a measurement" : run.Failure));
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                // 조합 하나의 실패(빌드 예외, 처음부터 컴파일되지 않음)로 그때까지 잰 결과를 버리지 않는다.
                rows.Add(UnityProfileRow.Failed(profile, ExceptionExitCode, error.Message));
            }
        }

        Console.Write(UnityProfile.Table(moduleName, rows));
        if (rows.Any(row => row.Succeeded && !row.ProcessMeasured))
        {
            Console.WriteLine("CPU time and peak memory are measured on Windows hosts only.");
        }

        string report = Path.Combine(output, "UnityProfile", moduleName + ".unity-profile.json");
        AtomicFile.WriteIfChanged(report, JsonSerializer.Serialize(new { module = moduleName, target, configuration = configuration.ToString(), rows },
            BuildCommand.JsonOptions));
        Console.WriteLine($"{Name}: wrote {report}");
        int failures = rows.Count(row => !row.Succeeded);
        if (failures != 0)
        {
            await Console.Error.WriteLineAsync($"{Name}: {failures} of {rows.Count} cases failed.");
            return FailedCasesExitCode;
        }

        return 0;
    }

    // 쉼표로 나눈 양의 정수 목록이다. 옵션이 없으면 기본값이다. 빈 항목(8,,16)과 0은 거부한다.
    private static IReadOnlyList<int> Values(Dictionary<string, string> options, string name, IReadOnlyList<int> defaults)
    {
        if (!options.TryGetValue(name, out string? text))
        {
            return defaults;
        }

        var values = new List<int>();
        foreach (string part in text.Split(',', StringSplitOptions.TrimEntries))
        {
            if (!int.TryParse(part, NumberStyles.None, CultureInfo.InvariantCulture, out int value) || value < 1)
            {
                throw new ArgumentException($"--{name} must be a comma-separated list of positive integers: {text}");
            }

            values.Add(value);
        }

        return values;
    }
}
