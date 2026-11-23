using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>엔진 타깃 하나의 계획 입력이다. Modules·BuildRoots는 메타 배치를 거친 모듈과 빌드 근이다.</summary>
internal sealed record EngineLayout(DescriptorModuleSet PluginSet, MetadataPlan Metadata, string[] BuildRoots);

/// <summary>
/// 엔진 저장소 타깃의 부작용 없는 계획 함수다(Docs/Design/ProjectFiles.ko.md 8.1절, PJD32). <c>build</c>·<c>test</c>·<c>sdk</c>와
/// <c>projectfiles</c>가 같은 함수로 규칙을 해석하고 요청을 만든다 — 둘이 갈라지면 IDE가 빌드와 다른 명령을 본다.
/// 이 함수들은 디렉터리를 만들거나 잠금을 잡거나 보고서를 쓰지 않는다(<see cref="Layout"/>의 createDirectories가 false일 때).
/// </summary>
internal static class EnginePlanning
{
    /// <summary>
    /// 타깃이 켠 엔진 플러그인을 적용하고(Docs/Design/BuildTargets 3.9절) 모듈 소유 메타데이터를 배치한다. createDirectories가
    /// false면 생성 include 디렉터리를 만들지 않는다(IDE 계획).
    /// </summary>
    public static EngineLayout Layout(LoadedRules rules, BuildContext context, string root, NativeToolchain toolchain,
        NativeConfiguration configuration, bool createDirectories)
    {
        var pluginSet = DescriptorModuleSet.Resolve(DescriptorOwner.ForEngine(rules.Target.Name, rules.Target.Plugins, root), rules.Plugins,
            toolchain.Platform, configuration, rules.Target.Kind);
        var ruleModules = pluginSet.LinkIntoPrimary(pluginSet.Apply(rules.Modules, rules.Target.Modules), rules.Target.Modules);
        string[] buildRoots = [.. rules.Target.Modules.Concat(pluginSet.IncludedPluginModules).Distinct(StringComparer.OrdinalIgnoreCase)];
        return new(pluginSet, MetadataLayout.Apply(context, ruleModules, buildRoots, createDirectories, rules.Modules), buildRoots);
    }

    /// <summary>엔진 타깃의 빌드 요청이다. 빌드 방식(유니티·PCH·작업 파일 분리·시간 측정·계획만)은 호출자가 덧붙인다.</summary>
    public static NativeBuildRequest Request(NativeToolchain toolchain, NativeConfiguration configuration, TargetRules target, string root,
        string output, IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<string> targets, EngineVersion engineVersion, int jobs) => new()
        {
            Toolchain = toolchain,
            Configuration = configuration,
            LogCompiledMinimum = target.LogCompiledMinimum,
            LinkType = target.LinkType,
            Target = target.Kind,
            TargetName = target.Name,
            RootDirectory = root,
            OutputDirectory = output,
            Modules = modules,
            Targets = targets,
            MaxParallelism = jobs,
            ContextFingerprint = "rocks-build-rules-v1",
            EngineVersion = engineVersion
        };

    /// <summary>엔진 타깃의 기본 출력 디렉터리다(<c>out/dotnet-&lt;플랫폼&gt;-&lt;타깃&gt;-&lt;구성&gt;</c>).</summary>
    public static string DefaultOutput(string root, string platformName, string targetName, NativeConfiguration configuration) =>
        Path.Combine(root, "out", $"dotnet-{platformName}-{targetName.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}");
}
