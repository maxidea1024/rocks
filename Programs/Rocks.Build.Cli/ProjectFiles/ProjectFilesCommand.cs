using System.Text.Json;
using Microsoft.VisualStudio.SolutionPersistence.Model;
using Microsoft.VisualStudio.SolutionPersistence.Serializer;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// <c>projectfiles</c> 명령이다(Docs/Design/ProjectFiles.ko.md). 엔진 저장소의 모든 타깃 × 구성을 IDE 계획 모드로 계획해 Visual Studio
/// 프로젝트 파일 모델을 만들고 검사한 뒤, 루트 <c>Rocks.slnx</c>와 <c>out/ProjectFiles/</c>에 쓴다(바뀐 파일만).
/// 사용 예: <c>Rocks.Build.Cli projectfiles --root .</c>. 게임은 <c>projectfiles --project &lt;게임&gt;</c>이다(<see cref="GameProjectFiles"/>).
/// <list type="bullet">
/// <item><c>--root</c>: 저장소 루트(기본 현재 디렉터리). <c>--compiler</c>·<c>--rules</c>: build와 같다.</item>
/// <item><c>--model-only on</c>: 모델을 만들고 검사만 하며 아무 파일도 쓰지 않는다.</item>
/// <item><c>--compare-compile-db &lt;compile_commands.json&gt; --target &lt;T&gt; --configuration &lt;C&gt;</c>: 빌드한 출력의 논리 명령과 IDE 계획의
/// 논리 명령이 같은지 본다(16절 V1). object 경로(<c>/Fo</c>)는 변형 디렉터리가 달라 비교에서 뺀다.</item>
/// </list>
/// 미분류 인자, 모델 검사 불일치, 비교 불일치는 오류다(종료 코드 1).
/// </summary>
internal static class ProjectFilesCommand
{
    public const string Name = "projectfiles";

    // 이 명령만 쓰는 옵션이다(공통 파서가 받는 이름 목록에 더한다).
    internal static readonly string[] Options = ["model-only", "compare-compile-db"];

    // IntelliSense에 쓰는 구성이다. Coverage는 측정 스크립트용이라 넣지 않는다(6.3절).
    private static readonly NativeConfiguration[] Configurations =
        [NativeConfiguration.Debug, NativeConfiguration.Development, NativeConfiguration.Release, NativeConfiguration.Asan];

    /// <summary>프로젝트 파일에 드는 구성인가(Coverage는 아니다, 6.3절).</summary>
    public static bool IsIdeConfiguration(NativeConfiguration configuration) => Configurations.Contains(configuration);

    // 경로 비교 규칙이다(Windows는 대소문자 무시).
    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    // 파일 소유의 동률을 가르는 기준 구성이다(6.2절).
    private const NativeConfiguration BaseConfiguration = NativeConfiguration.Development;

    // 엔진 프로젝트에 드는 루트다(저장소 기준).
    private static readonly string[] EngineRoots = ["Core", "Engine"];

    // Android 구성을 넣을 때 NDK를 찾는 ABI·API다(CLI 기본값과 같다, 6.3절).
    private const string AndroidAbi = "arm64";

    private const int AndroidApi = 26;

    // ClangCL 도구 모음을 주는 Visual Studio 구성 요소다(8.5절).
    private const string ClangToolsetComponent = "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset";

    // 생성 프로젝트가 요구하는 Visual Studio 판이다(vswhere 범위, PJD1 — Visual Studio 2026).
    private const string MinimumVisualStudioVersion = "[18.0,)";

    public static async Task<int> RunAsync(string[] args, CancellationToken cancellationToken)
    {
        Dictionary<string, string> options = BuildCommand.Parse(args);
        // 게임 모드(Docs/Design/ProjectFiles.ko.md 10.4절): 설치 SDK에 연결한 게임 프로젝트의 솔루션을 만든다.
        if (options.ContainsKey("project"))
        {
            return await GameProjectFiles.RunAsync(options, cancellationToken);
        }

        var allowed = new HashSet<string>(["root", "compiler", "rules", "model-only", "compare-compile-db", "target", "configuration"], StringComparer.Ordinal);
        foreach (string name in options.Keys.Where(name => !allowed.Contains(name)))
        {
            throw new ArgumentException($"Option --{name} is not supported by '{Name}'.");
        }

        bool modelOnly = options.GetValueOrDefault("model-only", "off") switch
        {
            "on" => true,
            "off" => false,
            _ => throw new ArgumentException("model-only must be on or off.")
        };
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
        (List<IdeTargetPlan> plans, Dictionary<string, ProjectFilesTarget> targets) = Plan(root, toolchain, rulesPath, engineVersion);
        double planning = System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds;

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
        if (compare is not null)
        {
            failures += Compare(compare, plans.Single(plan => plan.Target == options["target"] &&
                string.Equals(plan.Configuration, options["configuration"], StringComparison.OrdinalIgnoreCase)));
        }

        int checkedValues = model.Projects.Sum(project => project.Sources.Sum(file => file.Choices.Values.Count(choice => choice is not null)));
        Console.WriteLine($"Project files model: {targets.Count} targets, {plans.Count} IDE plans (up to {Configurations.Length} configurations each) " +
            $"({planning:F0} ms), {checkedValues} (file, configuration) values checked against their commands, " +
            $"{model.DefinitionCopies.Count} definitions header copies.");
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

        if (modelOnly)
        {
            return 0;
        }

        bool android = await AndroidAvailableAsync(cancellationToken);
        var input = new ProjectFilesInput(root, projectFiles, Path.Combine(root, "Rocks.slnx"), model, targets,
            [.. Configurations.Select(configuration => configuration.ToString())], android, ToolchainOf(toolchain),
            await CSharpProjectsAsync(root, cancellationToken));
        SortedDictionary<string, byte[]> files = VisualStudioProjects.Render(input);
        var fingerprints = ProjectFilesOutput.Fingerprints(plans, targets, ProjectFilesOutput.ToolchainHash(projectFiles, files));
        ProjectFilesWriteResult written = await ProjectFilesOutput.WriteAsync(root, projectFiles, input.SolutionPath, files, fingerprints,
            toolchain.CompilerPath, cancellationToken);
        Console.WriteLine($"Wrote {Path.Combine(root, "Rocks.slnx")} and {projectFiles}: {written.Written} written, {written.Unchanged} unchanged, " +
            $"{written.Removed} removed{(android ? "" : " (no Android NDK found: Android configurations left out)")}; " +
            $"{System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0} ms in total.");
        foreach (string note in ProjectFilesStaleness.GeneratedHeaderNotes(model, plans))
        {
            Console.WriteLine("note: " + note);
        }

        if (await ClangToolsetWarningAsync(toolchain, cancellationToken) is string warning)
        {
            Console.WriteLine(warning);
        }

        return 0;
    }

    /// <summary>
    /// 엔진 저장소의 모든 타깃 × 구성을 IDE 계획 모드로 계획한다(8.1절). 계획끼리는 독립이라 병렬로 돌고, 결과 순서는 (타깃, 구성) 순서다.
    /// </summary>
    internal static (List<IdeTargetPlan> Plans, Dictionary<string, ProjectFilesTarget> Targets) Plan(string root, NativeToolchain toolchain,
        string rulesPath, EngineVersion engineVersion)
    {
        const string platformName = "windows";
        var listing = new BuildContext(root, EnginePlanning.DefaultOutput(root, platformName, Name, BaseConfiguration),
            toolchain.Platform, toolchain.Architecture, BaseConfiguration);
        TargetRules[] targets = [.. RuleLoader.LoadAll(rulesPath, listing).Targets.OrderBy(target => target.Name, StringComparer.Ordinal)];
        IReadOnlyDictionary<string, (string Directory, string Name)> definitions = TargetDefinitions.Find(root, targets);
        (TargetRules Target, NativeConfiguration Configuration)[] work =
            [.. targets.SelectMany(target => Configurations.Where(configuration => LinkTypes.SupportsConfiguration(target.LinkType, configuration))
                .Select(configuration => (target, configuration)))];
        var plans = new IdeTargetPlan[work.Length];
        var executables = new string?[work.Length];
        Parallel.For(0, work.Length, index =>
        {
            (TargetRules target, NativeConfiguration configuration) = work[index];
            string output = EnginePlanning.DefaultOutput(root, platformName, target.Name, configuration);
            var context = new BuildContext(root, output, toolchain.Platform, toolchain.Architecture, configuration);
            LoadedRules rules = RuleLoader.Load(rulesPath, target.Name, context);
            EngineLayout layout = EnginePlanning.Layout(rules, context, root, toolchain, configuration, createDirectories: false);
            NativeBuildRequest request = EnginePlanning.Request(toolchain, configuration, rules.Target, root, output, layout.Metadata.Modules,
                layout.BuildRoots, engineVersion, 1);
            IdePlan plan = IdePlanner.Plan(request);
            (string directory, string definitionName) = definitions[target.Name];
            plans[index] = new(target.Name, directory, target.Type.ToString(), configuration.ToString(), plan, definitionName);
            string? primary = plan.ModuleOutputs.GetValueOrDefault(rules.Target.Modules[0]);
            executables[index] = primary is not null && primary.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? primary : null;
        });
        var result = new Dictionary<string, ProjectFilesTarget>(StringComparer.Ordinal);
        foreach (TargetRules target in targets)
        {
            var byConfiguration = new SortedDictionary<string, string?>(StringComparer.Ordinal);
            for (int index = 0; index < work.Length; ++index)
            {
                if (work[index].Target == target)
                {
                    byConfiguration[work[index].Configuration.ToString()] = executables[index];
                }
            }

            result[target.Name] = new(target.Name, target.Type.ToString(), byConfiguration);
        }

        return ([.. plans], result);
    }

    /// <summary>
    /// 이 컴퓨터의 도구 모음 값이다(8.5절). LLVM 루트와 주 버전은 컴파일러 resource 디렉터리(<c>&lt;LLVM&gt;/lib/clang/&lt;주&gt;</c>)에서 얻고,
    /// 모양이 다르면 오류다. 시스템 include는 도구 모음 환경의 <c>INCLUDE</c>다.
    /// </summary>
    internal static ProjectFilesToolchain ToolchainOf(NativeToolchain toolchain)
    {
        if (!toolchain.OwnedRoots.TryGetValue("resource", out string? resource))
        {
            throw new ArgumentException("The compiler resource directory is unknown; IntelliSense needs LLVMInstallDir.");
        }

        string version = Path.GetFileName(Path.TrimEndingDirectorySeparator(resource));
        string? clang = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(resource));
        string? lib = clang is null ? null : Path.GetDirectoryName(clang);
        if (clang is null || lib is null || !string.Equals(Path.GetFileName(clang), "clang", StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetFileName(lib), "lib", StringComparison.OrdinalIgnoreCase) || !Directory.Exists(Path.Combine(resource, "include")))
        {
            throw new ArgumentException($"Compiler resource directory {resource} is not <LLVM>/lib/clang/<major>/ with an include directory; " +
                "cannot set LLVMInstallDir for IntelliSense (Docs/Design/ProjectFiles.ko.md section 8.5).");
        }

        string[] includes = [.. (toolchain.Environment.GetValueOrDefault("INCLUDE") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)];
        string? asan = toolchain.RuntimeFiles.Count == 1 ? Path.GetDirectoryName(toolchain.RuntimeFiles[0]) : null;
        return new(toolchain.CompilerPath, Path.GetDirectoryName(lib)!, version, includes, asan, Path.Combine(resource, "include"));
    }

    // Android NDK가 있으면 Android 구성을 넣는다(6.3절).
    private static async Task<bool> AndroidAvailableAsync(CancellationToken cancellationToken)
    {
        try
        {
            await NativeToolchains.DiscoverAndroidAsync(AndroidAbi, AndroidApi, null, cancellationToken);
            return true;
        }
        catch (AndroidNdkNotConfiguredException)
        {
            return false;
        }
    }

    // C# 프로젝트 목록의 단일 출처는 Build/Format/Rocks.Format.slnx다(6.4절).
    internal static async Task<IReadOnlyList<string>> CSharpProjectsAsync(string root, CancellationToken cancellationToken)
    {
        string format = Path.Combine(root, "Build", "Format", "Rocks.Format.slnx");
        SolutionModel solution = await SolutionSerializers.SlnXml.OpenAsync(format, cancellationToken);
        return [.. solution.SolutionProjects.Select(project => Path.GetRelativePath(root, Path.GetFullPath(project.FilePath, Path.GetDirectoryName(format)!))
            .Replace('\\', '/')).Order(StringComparer.Ordinal)];
    }

    /// <summary>ClangCL 도구 모음 구성 요소가 없으면 그 경고 문구, 있으면 null이다(8.5절, 엔진·게임 모드 공통).</summary>
    internal static async Task<string?> ClangToolsetWarningAsync(NativeToolchain toolchain, CancellationToken cancellationToken) =>
        await ClangToolsetInstalledAsync(toolchain, cancellationToken) ? null
            : $"warning: Visual Studio component {ClangToolsetComponent} (C++ Clang tools for Windows) is not installed; " +
                "IntelliSense needs the ClangCL platform toolset (NMake builds work without it).";

    // vswhere로 ClangCL 도구 모음 구성 요소가 설치된 Visual Studio가 있는지 본다(8.5절).
    private static async Task<bool> ClangToolsetInstalledAsync(NativeToolchain toolchain, CancellationToken cancellationToken)
    {
        string installer = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Microsoft Visual Studio", "Installer", "vswhere.exe");
        if (!File.Exists(installer))
        {
            return false;
        }

        using var probe = new ProbeDirectory();
        var result = await ProcessRunner.RunAsync(new(installer, ["-latest", "-products", "*", "-version", MinimumVisualStudioVersion, "-requires",
                ClangToolsetComponent, "-property", "installationPath"],
            probe.Path, toolchain.Environment), cancellationToken);
        return result.ExitCode == 0 && result.StdOut.Trim().Length != 0;
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
            string.Join('\u001f', CompileObjectArguments.Without([.. arguments]));
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
