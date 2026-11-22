using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>
/// <c>build --files</c>·<c>game --files</c>의 CLI 쪽이다(Docs/Design/ProjectFiles.ko.md 7.4절). 메타 생성까지 평소처럼 한 뒤 호출하고,
/// 고른 소스의 논리 명령을 <see cref="SingleFileCompile"/>로 돌린다. Visual Studio Ctrl+F7이 <c>IdeBuild.ps1 -Action Compile</c>로 부른다.
/// </summary>
internal static class SingleFileCommand
{
    /// <summary>옵션 이름이다(값은 <c>;</c>로 나눈 소스 경로).</summary>
    public const string Option = "files";

    /// <summary>파일 하나 컴파일을 돌리고 컴파일러 출력을 낸다. 모두 성공하면 0, 하나라도 실패하면 1이다.</summary>
    public static async Task<int> RunAsync(NativeBuildRequest request, string list, string root, CancellationToken cancellationToken,
        Func<string, IReadOnlyList<string>>? compiledBy = null)
    {
        IReadOnlyList<SingleFileResult> results = await SingleFileCompile.RunAsync(request, SingleFileCompile.ParseList(list, root), cancellationToken,
            compiledBy);
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

    /// <summary>
    /// 엔진 저장소에서 같은 구성으로 그 소스를 컴파일하는 다른 타깃 이름이다(1단계 원천만 컴파일하므로 오류 안내에 쓴다). 다른 타깃마다 IDE 계획을
    /// 세운다(쓰기 없음) — 실패할 때만 부른다.
    /// </summary>
    internal static Func<string, IReadOnlyList<string>> EngineTargets(string root, string rulesPath, string current, NativeToolchain toolchain,
        NativeConfiguration configuration, EngineVersion engineVersion) => source =>
    {
        string platformName = OperatingSystem.IsWindows() ? "windows" : "macos";
        var listing = new BuildContext(root, EnginePlanning.DefaultOutput(root, platformName, current, configuration), toolchain.Platform,
            toolchain.Architecture, configuration);
        var found = new List<string>();
        foreach (TargetRules target in RuleLoader.LoadAll(rulesPath, listing).Targets.OrderBy(target => target.Name, StringComparer.Ordinal))
        {
            if (string.Equals(target.Name, current, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            try
            {
                string output = EnginePlanning.DefaultOutput(root, platformName, target.Name, configuration);
                var context = new BuildContext(root, output, toolchain.Platform, toolchain.Architecture, configuration);
                LoadedRules rules = RuleLoader.Load(rulesPath, target.Name, context);
                EngineLayout layout = EnginePlanning.Layout(rules, context, root, toolchain, configuration, createDirectories: false);
                IdePlan plan = IdePlanner.Plan(EnginePlanning.Request(toolchain, configuration, rules.Target, root, output, layout.Metadata.Modules,
                    layout.BuildRoots, engineVersion, 1));
                if (plan.Compiles.Any(compile => string.Equals(Path.GetFullPath(compile.Source), source, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)))
                {
                    found.Add(target.Name);
                }
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                // 안내 단계다: 이 구성에서 계획할 수 없는 타깃(플랫폼·플러그인 조건 등)은 빼고 원래 오류를 그대로 낸다.
            }
        }

        return found;
    };

    /// <summary>게임에서 같은 구성으로 그 소스를 컴파일하는 다른 게임 타깃 이름이다(엔진과 같은 안내, 실패할 때만 부른다).</summary>
    internal static Func<string, IReadOnlyList<string>> GameTargets(string project, string sdk, string rulesPath, string current,
        Descriptors.ProjectDescriptor descriptor, IReadOnlyList<Descriptors.PluginDescriptor> plugins, NativeToolchain toolchain,
        NativeConfiguration configuration, IReadOnlyList<SdkModule> sdkModules, EngineVersion engineVersion) => source =>
    {
        var found = new List<string>();
        var listing = new BuildContext(project, GameProjectLayout.DefaultOutput(project, current, configuration), toolchain.Platform, toolchain.Architecture,
            configuration, sdk);
        foreach (TargetRules target in RuleLoader.LoadAll(rulesPath, listing).Targets.OrderBy(target => target.Name, StringComparer.Ordinal))
        {
            if (string.Equals(target.Name, current, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            try
            {
                string output = GameProjectLayout.DefaultOutput(project, target.Name, configuration);
                var context = new BuildContext(project, output, toolchain.Platform, toolchain.Architecture, configuration, sdk);
                LoadedRules rules = RuleLoader.Load(rulesPath, target.Name, context);
                GameLayout layout = GamePlanning.Layout(rules, descriptor, plugins, context, toolchain, configuration, sdk, sdkModules, createDirectories: false);
                IdePlan plan = IdePlanner.Plan(GamePlanning.Request(toolchain, configuration, project, output, layout.Metadata.Modules, layout.Targets, sdk,
                    engineVersion, 1));
                if (plan.Compiles.Any(compile => string.Equals(Path.GetFullPath(compile.Source), source,
                    OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)))
                {
                    found.Add(target.Name);
                }
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                // 안내 단계다: 계획할 수 없는 타깃은 뺀다.
            }
        }

        return found;
    };
}
