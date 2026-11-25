using System.Text.Json;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 게임 모드 <c>projectfiles --project &lt;게임 디렉터리 또는 .rocksproject&gt; [--sdk &lt;SDK&gt;] [--compiler &lt;clang-cl&gt;] [--model-only on]</c>이다
/// (Docs/Design/ProjectFiles.ko.md 10.4절). 게임의 모든 타깃 × (SDK 구성 ∩ IDE 구성)을 IDE 계획 모드로 계획해 <c>&lt;게임&gt;/&lt;이름&gt;.slnx</c>와
/// <c>&lt;게임&gt;/out/ProjectFiles/</c>를 쓴다(바뀐 파일만). SDK는 게임 빌드와 같은 연결 해석으로 고른다(구성 조건 없음).
/// 생성은 고른 SDK의 CLI가 한다(다른 CLI가 받으면 그 CLI를 하위 프로세스로 부른다 — 게임 규칙이 SDK의 Rocks.Build를 참조한다). 시작부터
/// <c>out/ProjectFiles</c>의 호출 잠금을 쥐고, 규칙 어셈블리는 <c>out/ProjectFiles/Rules</c>(model-only는 임시 디렉터리)에 컴파일한다. 빌드 출력·
/// 보고서는 만들지 않는다.
/// </summary>
internal static class GameProjectFiles
{
    /// <summary><c>associate</c>가 연결은 바꿨지만 프로젝트 파일을 다시 만들지 못했을 때의 종료 코드다(CLI 예외와 같은 2).</summary>
    public const int AssociationKeptExitCode = 2;

    /// <summary>SDK 공개 헤더만 담는 탐색용 프로젝트 이름이다.</summary>
    public const string HeaderProjectName = "RocksSdk";

    // IntelliSense에 쓰는 구성 순서다(엔진 모드와 같다, 6.3절).
    private static readonly NativeConfiguration[] IdeConfigurations =
        [NativeConfiguration.Debug, NativeConfiguration.Development, NativeConfiguration.Release, NativeConfiguration.Asan];

    /// <summary>게임 프로젝트 파일 디렉터리다(<c>&lt;게임&gt;/out/ProjectFiles</c>).</summary>
    public static string ProjectFilesDirectory(string project) => Path.Combine(project, "out", "ProjectFiles");

    public static async Task<int> RunAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new ArgumentException($"'{ProjectFilesCommand.Name}' generates Visual Studio project files and runs on Windows hosts only.");
        }

        var allowed = new HashSet<string>(["project", "sdk", "compiler", "model-only"], StringComparer.Ordinal);
        foreach (string option in options.Keys.Where(key => !allowed.Contains(key)))
        {
            throw new ArgumentException($"Option --{option} is not supported by '{ProjectFilesCommand.Name} --project'.");
        }

        bool modelOnly = options.GetValueOrDefault("model-only", "off") switch
        {
            "on" => true,
            "off" => false,
            _ => throw new ArgumentException("model-only must be on or off.")
        };
        string path = Path.GetFullPath(options["project"]);
        string project = PathSpelling.OnDisk(File.Exists(path) ? Path.GetDirectoryName(path)! : path);
        options["project"] = project;
        var started = System.Diagnostics.Stopwatch.GetTimestamp();
        await InstalledSdk.ResolveSdkOptionAsync(options, cancellationToken, anyConfiguration: true);
        string sdk = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(options["sdk"])));
        if (DirectoryPath.IsAtOrUnder(project, sdk) || DirectoryPath.IsAtOrUnder(sdk, ProjectFilesDirectory(project)))
        {
            throw new ArgumentException("The game project cannot be inside the installed SDK, and the SDK cannot be inside the game's out/ProjectFiles.");
        }

        // 고른 SDK의 CLI가 아니면 그 CLI로 위임한다(Docs/Design/ProjectFiles.ko.md 10.4절).
        string sdkCli = Path.Combine(sdk, "Tools", "Rocks.Build.Cli", "Rocks.Build.Cli.dll");
        if (!DirectoryPath.IsAtOrUnder(typeof(GameProjectFiles).Assembly.Location, Path.GetDirectoryName(sdkCli)!))
        {
            if (!File.Exists(sdkCli))
            {
                throw new ArgumentException($"{sdk} is not an installed SDK with a build CLI ({sdkCli} not found).");
            }

            string[] forwarded = ["projectfiles", "--project", project, "--sdk", sdk, .. options.Where(pair => pair.Key is "compiler" or "model-only")
                .SelectMany(pair => new[] { "--" + pair.Key, pair.Value })];
            Console.WriteLine($"Generating with the CLI of the installed SDK: {sdkCli}");
            using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("dotnet", [sdkCli, .. forwarded])
            {
                UseShellExecute = false
            }) ?? throw new InvalidOperationException("Could not start " + sdkCli);
            await process.WaitForExitAsync(cancellationToken);
            return process.ExitCode;
        }

        NativeToolchain toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        NativeConfiguration[] configurations = Configurations(InstalledSdk.ManifestConfigurations(sdk));
        if (configurations.Length == 0)
        {
            throw new ArgumentException($"The SDK {sdk} has no IDE configuration ({string.Join(", ", IdeConfigurations)}); it has " +
                $"{string.Join(", ", InstalledSdk.ManifestConfigurations(sdk))}.");
        }

        // IDE 구성마다 manifest 구조를 검증하고 내용 해시는 한 번만 한다(빌드는 구성마다 다시 검증한다).
        SdkManifest manifest = InstalledSdk.Verify(sdk, configurations[0], toolchain);
        foreach (NativeConfiguration other in configurations.Skip(1))
        {
            InstalledSdk.VerifyStamped(sdk, other, toolchain, hashContents: false);
        }

        EngineVersion engineVersion = InstalledSdk.EngineVersionOf(manifest);
        ProjectDescriptor descriptor = GamePlanning.Descriptor(project, engineVersion);
        string name = Path.GetFileNameWithoutExtension(descriptor.FilePath);
        if (string.Equals(name, HeaderProjectName, StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException($"{descriptor.FilePath}: a game cannot be named {name}; the solution uses that name for the SDK header project.");
        }

        string projectFiles = ProjectFilesDirectory(project);
        // 시작부터 생성 영역의 호출 잠금을 쥔다(규칙 컴파일·쓰기, 동시 생성은 차례로). model-only는 게임 디렉터리에 쓰지 않는다.
        if (!modelOnly)
        {
            System.IO.Directory.CreateDirectory(projectFiles);
        }

        await using FileStream? generationLock = modelOnly ? null : await OutputInvocationLock.AcquireAsync(projectFiles, cancellationToken);
        string rulesDirectory = modelOnly ? ModelOnlyRulesDirectory() : Path.Combine(projectFiles, "Rules");
        try
        {
            return await GenerateAsync(project, sdk, name, projectFiles, rulesDirectory, descriptor, manifest, configurations, toolchain, engineVersion, modelOnly,
                started, cancellationToken);
        }
        finally
        {
            if (modelOnly)
            {
                TryDelete(rulesDirectory);
            }
        }
    }

    // model-only 규칙 디렉터리의 접두다(<접두><pid>-<guid>).
    private const string ModelOnlyRulesPrefix = "rocks-game-rules-";

    // model-only 규칙 어셈블리를 둘 임시 디렉터리다. 규칙 어셈블리는 기본 load context에 올라 이 프로세스가 끝날 때까지 파일이 잠기므로 끝에서는
    // best-effort로 지우고, 여기서 프로세스가 없는 pid의 남은 디렉터리를 지운다(Docs/Design/ProjectFiles.ko.md 11.6절).
    private static string ModelOnlyRulesDirectory()
    {
        foreach (string stale in System.IO.Directory.EnumerateDirectories(Path.GetTempPath(), ModelOnlyRulesPrefix + "*"))
        {
            string[] parts = Path.GetFileName(stale)[ModelOnlyRulesPrefix.Length..].Split('-', 2);
            if (parts.Length == 2 && int.TryParse(parts[0], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out int pid) &&
                !Running(pid))
            {
                TryDelete(stale);
            }
        }

        string directory = Path.Combine(Path.GetTempPath(), $"{ModelOnlyRulesPrefix}{Environment.ProcessId}-{Guid.NewGuid():N}");
        System.IO.Directory.CreateDirectory(directory);
        return directory;
    }

    private static bool Running(int pid)
    {
        try
        {
            using var process = System.Diagnostics.Process.GetProcessById(pid);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            // 다른 사용자·보호 프로세스라 상태를 알 수 없다. 지우지 않도록 실행 중으로 본다.
            return true;
        }
    }

    private static void TryDelete(string directory)
    {
        try
        {
            System.IO.Directory.Delete(directory, recursive: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            // 이 프로세스가 올린 규칙 어셈블리가 잠겨 있다. 다음 model-only 생성이 지운다.
        }
    }

    /// <summary>SDK가 가진 구성 이름 중 IDE 구성을 구성 순서로 고른다(Debug·Development·Release·Asan, Coverage 등은 뺀다).</summary>
    internal static NativeConfiguration[] Configurations(IEnumerable<string> available)
    {
        var names = available.ToHashSet(StringComparer.Ordinal);
        return [.. IdeConfigurations.Where(configuration => names.Contains(configuration.ToString()))];
    }

    private static async Task<int> GenerateAsync(string project, string sdk, string name, string projectFiles, string rulesDirectory,
        ProjectDescriptor descriptor, SdkManifest manifest, NativeConfiguration[] configurations, NativeToolchain toolchain, EngineVersion engineVersion,
        bool modelOnly, long started, CancellationToken cancellationToken)
    {
        string[] ruleFiles = GameProjectLayout.RuleFiles(project, projectFiles, sdk);
        string? rulesPath = await GamePlanning.CompileRulesAsync(rulesDirectory, ruleFiles, sdk, toolchain, cancellationToken);
        if (rulesPath is null)
        {
            return 1;
        }

        NativeConfiguration baseConfiguration = configurations.Contains(NativeConfiguration.Development) ? NativeConfiguration.Development : configurations[0];
        (List<IdeTargetPlan> plans, Dictionary<string, ProjectFilesTarget> targets) = Plan(project, sdk, rulesPath, ruleFiles, descriptor, manifest,
            configurations, baseConfiguration, toolchain, engineVersion);
        double planning = System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds;
        IntelliSenseModel model = IntelliSenseModel.Create(plans, project, projectFiles, [], baseConfiguration.ToString(),
            new IdeModelLayout(name, HeaderProjectName, sdk));
        IReadOnlyList<string> mismatches = IntelliSenseModel.Verify(model, plans, projectFiles);
        foreach (string mismatch in mismatches)
        {
            Console.Error.WriteLine("Model check: " + mismatch);
        }

        Console.WriteLine($"Game project files model ({name}, SDK {sdk}): {targets.Count} targets x {configurations.Length} configurations = " +
            $"{plans.Count} IDE plans ({planning:F0} ms).");
        foreach (IdeProject entry in model.Projects)
        {
            Console.WriteLine($"  {entry.Name}: {entry.Configurations.Count} configurations, {entry.Sources.Count} sources, {entry.Headers.Count} headers, " +
                $"{entry.Bundles.Count} bundles.");
        }

        foreach (string notice in model.Notices)
        {
            Console.WriteLine("note: " + notice);
        }

        if (mismatches.Count != 0)
        {
            Console.Error.WriteLine($"Project files model check failed: {mismatches.Count} mismatch(es).");
            return 1;
        }

        if (modelOnly)
        {
            return 0;
        }

        string solution = Path.Combine(project, name + ".slnx");
        var input = new ProjectFilesInput(project, projectFiles, solution, model, targets, [.. configurations.Select(configuration => configuration.ToString())],
            false, ProjectFilesCommand.ToolchainOf(toolchain), [], new ProjectFilesGame(descriptor.FilePath, sdk, HeaderProjectName));
        SortedDictionary<string, byte[]> files = VisualStudioProjects.Render(input);
        var fingerprints = ProjectFilesOutput.Fingerprints(plans, targets, ProjectFilesOutput.ToolchainHash(projectFiles, files));
        ProjectFilesWriteResult written = await ProjectFilesOutput.WriteAsync(project, projectFiles, solution, files, fingerprints, toolchain.CompilerPath,
            cancellationToken, lockHeld: true);
        Console.WriteLine($"Wrote {solution} and {projectFiles}: {written.Written} written, {written.Unchanged} unchanged, {written.Removed} removed; " +
            $"{System.Diagnostics.Stopwatch.GetElapsedTime(started).TotalMilliseconds:F0} ms in total.");
        foreach (string note in ProjectFilesStaleness.GeneratedHeaderNotes(model, plans))
        {
            Console.WriteLine("note: " + note);
        }

        if (await ProjectFilesCommand.ClangToolsetWarningAsync(toolchain, cancellationToken) is string warning)
        {
            Console.WriteLine(warning);
        }

        return 0;
    }

    /// <summary>
    /// <c>associate</c>가 연결을 바꾼 뒤 부른다: 게임에 생성 상태 파일이 있으면 다시 만든다(생성할 때의 컴파일러). 없으면 0이다.
    /// 연결은 이미 바뀌었으므로 그대로 두고, 다시 만들기가 어떤 이유로든 실패하면 이유와 다시 만드는 방법을 알리고 2다(10.4절).
    /// </summary>
    public static async Task<int> RegenerateAsync(string projectPath, CancellationToken cancellationToken)
    {
        string full = Path.GetFullPath(projectPath);
        string project = File.Exists(full) ? Path.GetDirectoryName(full)! : full;
        ProjectFilesState? state = ProjectFilesOutput.Read(Path.Combine(ProjectFilesDirectory(project), ProjectFilesOutput.StateFileName));
        if (state is null)
        {
            return 0;
        }

        Console.WriteLine("Regenerating the game project files for the new association.");
        var options = new Dictionary<string, string>(StringComparer.Ordinal) { ["project"] = project };
        if (state.Compiler.Length != 0)
        {
            options["compiler"] = state.Compiler;
        }

        string? failure;
        try
        {
            int code = await RunAsync(options, cancellationToken);
            failure = code == 0 ? null : $"the generator exited with {code}";
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            failure = error.Message;
        }

        if (failure is null)
        {
            return 0;
        }

        Console.Error.WriteLine($"error: the association was changed but the game project files were not regenerated: {failure} Next: run " +
            $"Build/GenerateProjectFiles.ps1 -Project \"{project}\" once an SDK can be chosen.");
        return AssociationKeptExitCode;
    }

    // 게임의 모든 타깃 × 구성을 IDE 계획 모드로 계획한다(8.1절). 계획끼리는 독립이라 병렬로 돈다.
    private static (List<IdeTargetPlan> Plans, Dictionary<string, ProjectFilesTarget> Targets) Plan(string project, string sdk, string rulesPath,
        IReadOnlyList<string> ruleFiles, ProjectDescriptor descriptor, SdkManifest manifest, NativeConfiguration[] configurations,
        NativeConfiguration baseConfiguration, NativeToolchain toolchain, EngineVersion engineVersion)
    {
        var listing = new BuildContext(project, GameProjectLayout.DefaultOutput(project, GameProjectLayout.DefaultTarget(project), baseConfiguration),
            toolchain.Platform, toolchain.Architecture, baseConfiguration, sdk);
        TargetRules[] targets = [.. RuleLoader.LoadAll(rulesPath, listing).Targets.OrderBy(target => target.Name, StringComparer.Ordinal)];
        IReadOnlyDictionary<string, (string Directory, string Name)> definitions = TargetDefinitions.FromFiles(ruleFiles, targets);
        IReadOnlyList<PluginDescriptor> plugins = PluginCatalog.Discover([Path.Combine(project, "Plugins")]);
        (TargetRules Target, NativeConfiguration Configuration)[] work =
            [.. targets.SelectMany(target => configurations.Select(configuration => (target, configuration)))];
        var plans = new IdeTargetPlan[work.Length];
        var executables = new string?[work.Length];
        Parallel.For(0, work.Length, index =>
        {
            (TargetRules target, NativeConfiguration configuration) = work[index];
            string output = GameProjectLayout.DefaultOutput(project, target.Name, configuration);
            var context = new BuildContext(project, output, toolchain.Platform, toolchain.Architecture, configuration, sdk);
            LoadedRules rules = RuleLoader.Load(rulesPath, target.Name, context);
            GameLayout layout = GamePlanning.Layout(rules, descriptor, plugins, context, toolchain, configuration, sdk,
                manifest.Configurations[configuration.ToString()].Modules, createDirectories: false);
            IdePlan plan = IdePlanner.Plan(GamePlanning.Request(toolchain, configuration, project, output, layout.Metadata.Modules, layout.Targets, sdk,
                engineVersion, 1, rules.Target.Kind));
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
}
