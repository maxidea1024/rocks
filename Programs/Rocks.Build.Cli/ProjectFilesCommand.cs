using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// <c>projectfiles</c> 명령이다(Docs/Design/ProjectFiles.ko.md). 엔진 저장소의 모든 타깃 × 구성을 IDE 계획 모드로 계획해 Visual Studio
/// 프로젝트 파일 모델을 만든다. 지금(PJ0)은 <c>--model-only on</c>만 받는다: 모델을 만들고 검사한 뒤 요약을 출력하며 아무 파일도 쓰지
/// 않는다(프로젝트 파일 쓰기는 PJ1).
/// 사용 예: <c>Rocks.Build.Cli projectfiles --root . --model-only on</c>.
/// <list type="bullet">
/// <item><c>--root</c>: 저장소 루트(기본 현재 디렉터리). <c>--compiler</c>·<c>--rules</c>: build와 같다.</item>
/// <item><c>--compare-compile-db &lt;compile_commands.json&gt; --target &lt;T&gt; --configuration &lt;C&gt;</c>: 빌드한 출력의 논리 명령과 IDE 계획의
/// 논리 명령이 같은지 본다(16절 V1). object 경로(<c>/Fo</c>)는 변형 디렉터리가 달라 비교에서 뺀다.</item>
/// </list>
/// 미분류 인자, 왕복 불일치, 비교 불일치는 오류다(종료 코드 1).
/// </summary>
internal static class ProjectFilesCommand
{
    public const string Name = "projectfiles";

    // 이 명령만 쓰는 옵션이다(공통 파서가 받는 이름 목록에 더한다).
    internal static readonly string[] Options = ["model-only", "compare-compile-db"];

    // IntelliSense에 쓰는 구성이다. Coverage는 측정 스크립트용이라 넣지 않는다(6.3절).
    private static readonly NativeConfiguration[] Configurations =
        [NativeConfiguration.Debug, NativeConfiguration.Development, NativeConfiguration.Release, NativeConfiguration.Asan];

    // 경로 비교 규칙이다(Windows는 대소문자 무시).
    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    // 파일 소유의 동률을 가르는 기준 구성이다(6.2절).
    private const NativeConfiguration BaseConfiguration = NativeConfiguration.Development;

    // 엔진 프로젝트에 드는 루트다(저장소 기준).
    private static readonly string[] EngineRoots = ["Core", "Engine"];

    public static async Task<int> RunAsync(string[] args, CancellationToken cancellationToken)
    {
        Dictionary<string, string> options = BuildCommand.Parse(args);
        var allowed = new HashSet<string>(["root", "compiler", "rules", "model-only", "compare-compile-db", "target", "configuration"], StringComparer.Ordinal);
        foreach (string name in options.Keys.Where(name => !allowed.Contains(name)))
        {
            throw new ArgumentException($"Option --{name} is not supported by '{Name}'.");
        }

        if (options.GetValueOrDefault("model-only") != "on")
        {
            throw new ArgumentException($"'{Name}' writes no project files yet; pass --model-only on (Docs/Design/ProjectFiles.ko.md stage PJ1).");
        }

        if (!OperatingSystem.IsWindows())
        {
            throw new ArgumentException($"'{Name}' generates Visual Studio project files and runs on Windows hosts only.");
        }

        string? compare = options.GetValueOrDefault("compare-compile-db");
        bool comparing = compare is not null;
        if (comparing != (options.ContainsKey("target") && options.ContainsKey("configuration")))
        {
            throw new ArgumentException("--compare-compile-db requires --target and --configuration (and they are used only with it).");
        }

        string root = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory())));
        var started = System.Diagnostics.Stopwatch.GetTimestamp();
        EngineVersion engineVersion = EngineVersion.Read(EngineVersion.PathIn(root));
        NativeToolchain toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        string rulesPath = options.GetValueOrDefault("rules", BuildCommand.HostTool(root, "Rocks.Engine.Rules"));
        const string platformName = "windows";

        var listing = new BuildContext(root, EnginePlanning.DefaultOutput(root, platformName, "projectfiles", BaseConfiguration),
            toolchain.Platform, toolchain.Architecture, BaseConfiguration);
        TargetRules[] targets = [.. RuleLoader.LoadAll(rulesPath, listing).Targets.OrderBy(target => target.Name, StringComparer.Ordinal)];
        IReadOnlyDictionary<string, (string Directory, string Name)> definitions = TargetDefinitions.Find(root, targets);
        var plans = new List<IdeTargetPlan>();
        foreach (TargetRules target in targets)
        {
            foreach (NativeConfiguration configuration in Configurations)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string output = EnginePlanning.DefaultOutput(root, platformName, target.Name, configuration);
                var context = new BuildContext(root, output, toolchain.Platform, toolchain.Architecture, configuration);
                LoadedRules rules = RuleLoader.Load(rulesPath, target.Name, context);
                EngineLayout layout = EnginePlanning.Layout(rules, context, root, toolchain, configuration, createDirectories: false);
                NativeBuildRequest request = EnginePlanning.Request(toolchain, configuration, rules.Target, root, output, layout.Metadata.Modules,
                    layout.BuildRoots, engineVersion, 1);
                IdePlan plan = IdePlanner.Plan(request);
                (string directory, string definitionName) = definitions[target.Name];
                plans.Add(new(target.Name, directory, target.Type.ToString(), configuration.ToString(), plan, definitionName));
            }
        }

        string projectFiles = Path.Combine(root, "out", "ProjectFiles");
        IntelliSenseModel model = IntelliSenseModel.Create(plans, root, projectFiles, [.. EngineRoots.Select(name => Path.Combine(root, name))],
            BaseConfiguration.ToString());
        // 모델 검사(16절 V1): 묶음에 접힌 값을 (파일, 구성)마다 고른 명령과 대조한다.
        IReadOnlyList<string> mismatches = IntelliSenseModel.Verify(model, plans, projectFiles);
        foreach (string mismatch in mismatches)
        {
            Console.Error.WriteLine("Model check: " + mismatch);
        }

        int failures = mismatches.Count;
        int checkedCompiles = model.Projects.Sum(project => project.Sources.Sum(file => file.Choices.Values.Count(choice => choice is not null)));

        if (compare is not null)
        {
            failures += Compare(compare, plans.Single(plan => plan.Target == options["target"] &&
                string.Equals(plan.Configuration, options["configuration"], StringComparison.OrdinalIgnoreCase)));
        }

        Console.WriteLine($"Project files model: {targets.Length} targets x {Configurations.Length} configurations = {plans.Count} IDE plans, " +
            $"{checkedCompiles} (file, configuration) values checked against their commands, {model.DefinitionCopies.Count} definitions header copies, " +
            $"{System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0} ms.");
        foreach (IdeProject project in model.Projects)
        {
            int largest = project.DefaultBundle is int bundle ? project.Sources.Count(file => file.Bundle == bundle) : 0;
            Console.WriteLine($"  {project.Name}: {project.Configurations.Count} configurations, {project.Sources.Count} sources " +
                $"({project.Sources.Count(file => file.Bundle is null)} without IntelliSense), {project.Headers.Count} headers, " +
                $"{project.Bundles.Count} bundles (default bundle {largest} files).");
        }

        foreach (string notice in model.Notices)
        {
            Console.WriteLine("note: " + notice);
        }

        if (failures != 0)
        {
            Console.Error.WriteLine($"Project files model check failed: {failures} mismatch(es).");
            return 1;
        }

        return 0;
    }

    // 빌드한 출력의 compile_commands.json(같은 타깃·구성)과 IDE 계획의 논리 명령을 비교한다. 출력 아래 소스(생성 TU·공개 헤더 검사)는
    // IDE 대상이 아니므로 뺀다. 불일치 수를 돌려준다.
    private static int Compare(string path, IdeTargetPlan plan)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        // 한 소스를 여러 모듈이 컴파일하면 명령이 여럿이다. 소스마다 명령 목록(object 경로 제외)을 정렬해 비교한다.
        var built = new Dictionary<string, List<string>>(PathComparer);
        foreach (JsonElement entry in document.RootElement.EnumerateArray())
        {
            string file = Path.GetFullPath(entry.GetProperty("file").GetString()!);
            if (DirectoryPath.IsAtOrUnder(file, plan.Plan.OutputDirectory))
            {
                continue;
            }

            (built.TryGetValue(file, out var list) ? list : built[file] = []).Add(
                WithoutObject(entry.GetProperty("arguments").EnumerateArray().Skip(1).Select(value => value.GetString()!)));
        }

        static string WithoutObject(IEnumerable<string> arguments) =>
            string.Join('\u001f', arguments.Where(argument => !argument.StartsWith("/Fo", StringComparison.Ordinal)));
        int failures = 0;
        var seen = new HashSet<string>(PathComparer);
        foreach (var group in plan.Plan.Compiles.GroupBy(compile => Path.GetFullPath(compile.Source), PathComparer))
        {
            string source = group.Key;
            seen.Add(source);
            string[] ide = [.. group.Select(compile => WithoutObject(compile.Arguments)).Order(StringComparer.Ordinal)];
            if (!built.TryGetValue(source, out var commands))
            {
                Console.Error.WriteLine($"Compare: {source} is in the IDE plan but not in {path}.");
                ++failures;
            }
            else if (!commands.Order(StringComparer.Ordinal).SequenceEqual(ide, StringComparer.Ordinal))
            {
                Console.Error.WriteLine($"Compare: arguments differ for {source} ({ide.Length} IDE command(s), {commands.Count} built).");
                ++failures;
            }
        }

        foreach (string file in built.Keys.Where(file => !seen.Contains(file)))
        {
            Console.Error.WriteLine($"Compare: {file} is in {path} but not in the IDE plan.");
            ++failures;
        }

        Console.WriteLine($"Compared {seen.Count} sources of {plan.Target} {plan.Configuration} with {path}: {failures} mismatch(es).");
        return failures;
    }
}

/// <summary>
/// 타깃 정의 디렉터리를 찾는다(Docs/Design/ProjectFiles.ko.md 6.2절): 규칙 어셈블리와 같은 glob의 <c>*.Target.cs</c> 중 그 타깃 규칙 타입의
/// <c>class</c> 선언이 정확히 한 파일에 있어야 한다.
/// </summary>
internal static class TargetDefinitions
{
    // 규칙 어셈블리(Build/Rules/Rocks.Engine.Rules.csproj)가 *.Target.cs를 모으는 루트다. Programs/Rocks.*(C# 도구)는 제외한다.
    private static readonly string[] Roots = ["Core/Source", "Engine/Source", "Engine/Plugins", "Programs"];

    // 타깃 규칙 파일 이름의 끝이다.
    private const string TargetSuffix = ".Target.cs";

    /// <summary>타깃 이름 → (정의 디렉터리, 정의 파일 이름에서 <c>.Target.cs</c>를 뗀 것)이다.</summary>
    /// <exception cref="ArgumentException">규칙 타입의 선언이 정확히 한 파일에 있지 않다.</exception>
    public static IReadOnlyDictionary<string, (string Directory, string Name)> Find(string root, IReadOnlyList<TargetRules> targets)
    {
        string[] files = [.. Roots.Select(relative => Path.Combine(root, relative)).Where(Directory.Exists)
            .SelectMany(directory => Directory.EnumerateFiles(directory, "*.Target.cs", SearchOption.AllDirectories))
            .Where(file => !Path.GetRelativePath(root, file).Replace('\\', '/').StartsWith("Programs/Rocks.", StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)];
        var texts = files.ToDictionary(file => file, File.ReadAllText, StringComparer.Ordinal);
        var result = new Dictionary<string, (string Directory, string Name)>(StringComparer.Ordinal);
        foreach (TargetRules target in targets)
        {
            var declaration = new Regex(@"\bclass\s+" + Regex.Escape(target.GetType().Name) + @"\b", RegexOptions.CultureInvariant);
            string[] declaring = [.. files.Where(file => declaration.IsMatch(texts[file]))];
            if (declaring.Length != 1)
            {
                throw new ArgumentException($"Target {target.Name}: rule type {target.GetType().Name} must be declared in exactly one *.Target.cs " +
                    $"(found {declaring.Length}).");
            }

            result[target.Name] = (Path.GetDirectoryName(declaring[0])!, Path.GetFileName(declaring[0])[..^TargetSuffix.Length]);
        }

        return result;
    }
}
