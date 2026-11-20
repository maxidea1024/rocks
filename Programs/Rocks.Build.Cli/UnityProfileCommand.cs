using System.Globalization;
using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 조합 하나의 빌드에 얹는 측정 요청이다. <see cref="BuildCommand"/>의 build 흐름이 이 값을 보고 그 모듈만 재정의·재컴파일하고,
/// 끝나면 <see cref="Row"/>에 결과를 채운다.
/// </summary>
internal sealed class UnityProfileRun(string module, UnityProfileCase profile)
{
    public string Module { get; } = module;

    public UnityProfileCase Case { get; } = profile;

    public UnityProfileRow? Row { get; set; }
}

/// <summary>
/// <c>unity-profile</c> 명령이다(Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측"). 모듈 하나를 "유니티 끔"과
/// <c>UnityAnchorFiles</c>·묶음 상한 조합마다 처음부터 컴파일하고, 조합별 번역 단위 수·CPU 시간·벽시계 시간·최대 메모리 표를 낸다.
/// 사용 예: <c>Rocks.Build.Cli unity-profile --root . --module Core</c>.
/// <list type="bullet">
/// <item><c>--module</c>(필수): 재는 모듈. <c>--target</c>(기본 Foundation)이 빌드하는 모듈이어야 한다.</item>
/// <item><c>--anchor-files</c>·<c>--batch-kib</c>: 쉼표로 나눈 값 목록. 기본은 8,16,24,32와 256,384,512다.</item>
/// <item><c>--configuration</c>·<c>--compiler</c>·<c>--rules</c>·<c>--jobs</c>·<c>--output</c>: build와 같다. 출력 기본은
/// <c>out/unity-profile-&lt;타깃&gt;-&lt;구성&gt;</c>이어서 평소 빌드 출력을 건드리지 않는다.</item>
/// </list>
/// 먼저 의존 모듈을 빌드하는 준비 실행을 한 번 하고(측정하지 않는다), 그다음 조합마다 그 모듈의 object와 묶음 파일을 지우고 다시
/// 빌드한다. 표는 표준 출력과 <c>&lt;출력&gt;/UnityProfile/&lt;모듈&gt;.unity-profile.json</c>에 쓴다.
/// </summary>
internal static class UnityProfileCommand
{
    public const string Name = "unity-profile";

    private const string ModuleOption = "module";

    private const string AnchorFilesOption = "anchor-files";

    private const string BatchKibibytesOption = "batch-kib";

    private const string DefaultTarget = "Foundation";

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
        string output = options.GetValueOrDefault("output",
            Path.Combine(root, "out", $"{Name}-{target.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}"));

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

        // 준비 실행: 의존 모듈과 생성 코드를 빌드해 둔다. 뒤 조합의 벽시계 시간에 의존 모듈 빌드가 섞이지 않는다.
        Console.WriteLine($"{Name}: preparing dependencies of {module} ({cases.Count} cases follow)");
        int prepared = await BuildCommand.RunAsync([.. build], cancellationToken, new UnityProfileRun(module, cases[0]));
        if (prepared != 0)
        {
            return prepared;
        }

        var rows = new List<UnityProfileRow>();
        foreach (UnityProfileCase profile in cases)
        {
            Console.WriteLine($"{Name}: {module}: {profile.Name}");
            var run = new UnityProfileRun(module, profile);
            int exit = await BuildCommand.RunAsync([.. build], cancellationToken, run);
            if (exit != 0)
            {
                return exit;
            }

            rows.Add(run.Row ?? throw new InvalidOperationException($"The build did not report a measurement for {profile.Name}."));
        }

        Console.Write(UnityProfile.Table(module, rows));
        if (rows.Any(row => !row.ProcessMeasured))
        {
            Console.WriteLine("CPU time and peak memory are measured on Windows hosts only.");
        }

        string report = Path.Combine(output, "UnityProfile", module + ".unity-profile.json");
        AtomicFile.WriteIfChanged(report, JsonSerializer.Serialize(new { module, target, configuration = configuration.ToString(), rows }, BuildCommand.JsonOptions));
        Console.WriteLine($"{Name}: wrote {report}");
        return 0;
    }

    // 쉼표로 나눈 양의 정수 목록이다. 옵션이 없으면 기본값이다.
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
