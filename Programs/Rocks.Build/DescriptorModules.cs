namespace Rocks.Build.Descriptors;

/// <summary>
/// 기술자(.rocksproject·.rocksplugin)에 적힌 모듈 하나와, 이번 빌드에서 뺀 이유다.
/// ExcludedReason이 null이면 이번 빌드에 들어간다. FromPlugin은 플러그인 기술자가 선언한 모듈인지다.
/// </summary>
public sealed record DescribedModule(ModuleDescriptor Descriptor, string OwnerName, string OwnerFile, bool FromPlugin,
    string? ExcludedReason)
{
    public string Name => Descriptor.Name;

    public bool Included => ExcludedReason is null;
}

/// <summary>
/// 기술자로 이번 빌드(플랫폼·구성)에 넣을 모듈을 정하고, C# 모듈 규칙과 서로 맞는지 검사한다(UE
/// UnrealBuildTool이 .uproject·.uplugin의 Modules 목록으로 빌드할 모듈을 고르는 것과 같은 역할).
///
/// 사용 예(게임 빌드):
/// <code>
/// var project = DescriptorReader.ReadProject(projectFile);
/// var plugins = PluginCatalog.Discover([Path.Combine(project.RootDirectory, "Plugins")]);
/// var set = DescriptorModuleSet.Resolve(project, plugins, platform, configuration);
/// var modules = set.Apply(ruleModules, target.Modules);   // 빌드할 규칙 모듈만 남는다
/// </code>
///
/// 모듈을 빼는 규칙:
/// <list type="bullet">
/// <item>켜지지 않은 플러그인의 모듈(PluginCatalog.ResolveEnabled 기준).</item>
/// <item>Editor 모듈: 아직 에디터 타깃이 없으므로 항상 뺀다.</item>
/// <item>Developer 모듈: Release(배포)에서 뺀다. UE 5.6의 DeveloperTool(Shipping 외 모든 타깃)과 같은 의미다.
/// UE의 Developer(에디터·프로그램 타깃 전용)와는 다르다.</item>
/// <item>platforms에 이번 플랫폼이 없는 모듈.</item>
/// </list>
/// 오류(DescriptorException): 프로젝트가 이번 플랫폼을 targetPlatforms에 두지 않음, 여러 기술자가 같은 모듈 이름을
/// 선언, 규칙은 있는데 기술자가 없는 모듈, 기술자는 있는데 규칙이 없는 모듈, 빠진 모듈을 타깃이나 남은 모듈이 참조.
///
/// 규칙 계약: 규칙 로더는 이름을 알기 위해 모든 모듈 규칙의 Configure를 먼저 실행한 뒤 이 필터를 적용한다.
/// 따라서 규칙은 빠질 모듈(켜지지 않은 플러그인, 다른 플랫폼 전용)이라도 예외 없이 NativeModuleInput을
/// 돌려줘야 한다(소스 폴더를 플랫폼 조건으로 만들지 않는다).
/// </summary>
public sealed class DescriptorModuleSet
{
    private readonly Dictionary<string, DescribedModule> modules;

    private DescriptorModuleSet(ProjectDescriptor project, Dictionary<string, DescribedModule> modules,
        IReadOnlyList<PluginDescriptor> enabledPlugins)
    {
        Project = project;
        this.modules = modules;
        EnabledPlugins = enabledPlugins;
    }

    public ProjectDescriptor Project { get; }

    /// <summary>켠 플러그인(의존 순서)이다.</summary>
    public IReadOnlyList<PluginDescriptor> EnabledPlugins { get; }

    /// <summary>모든 기술자가 선언한 모듈(빠진 모듈 포함)이다. 이름 순이다.</summary>
    public IReadOnlyList<DescribedModule> Modules =>
        modules.Values.OrderBy(module => module.Name, StringComparer.Ordinal).ToList();

    /// <summary>
    /// 켠 플러그인의 모듈 중 이번 빌드에 들어가는 모듈 이름이다(이름 순). 타깃이 직접 참조하지 않아도 빌드
    /// 대상에 더한다(UE가 켠 플러그인 모듈을 타깃에 넣는 것과 같다).
    /// </summary>
    public IReadOnlyList<string> IncludedPluginModules =>
        Modules.Where(module => module.FromPlugin && module.Included).Select(module => module.Name).ToList();

    /// <summary>
    /// project와 찾은 플러그인(available)으로 모듈 집합을 정한다. available에는 켜지지 않은 플러그인도 넣는다(그
    /// 모듈도 규칙과 대조하고, 참조하면 "켜지지 않음"으로 알려 준다).
    /// </summary>
    public static DescriptorModuleSet Resolve(ProjectDescriptor project, IReadOnlyList<PluginDescriptor> available,
        NativePlatform platform, NativeConfiguration configuration)
    {
        if (!project.TargetPlatforms.Contains(platform))
            throw new DescriptorException(project.FilePath,
                $"the project does not target {platform} (targetPlatforms: {string.Join(", ", project.TargetPlatforms)}).");

        IReadOnlyList<PluginDescriptor> enabled = PluginCatalog.ResolveEnabled(project, available);
        var enabledNames = enabled.Select(plugin => plugin.Name).ToHashSet(StringComparer.Ordinal);
        var result = new Dictionary<string, DescribedModule>(StringComparer.OrdinalIgnoreCase);

        void Add(ModuleDescriptor module, string owner, string file, bool fromPlugin, string? pluginNotEnabled)
        {
            string? reason = pluginNotEnabled ?? module.Type switch
            {
                ModuleType.Editor => "Editor modules are not built yet (there is no editor target)",
                ModuleType.Developer when configuration == NativeConfiguration.Release => "Developer modules are not built in Release",
                _ => module.SupportsPlatform(platform) ? null : $"the module does not support {platform}"
            };
            if (!result.TryAdd(module.Name, new(module, owner, file, fromPlugin, reason)))
                throw new DescriptorException(file,
                    $"module '{module.Name}' is also declared by {result[module.Name].OwnerFile}.");
        }

        foreach (ModuleDescriptor module in project.Modules)
            Add(module, project.Name, project.FilePath, false, null);

        foreach (PluginDescriptor plugin in available.OrderBy(plugin => plugin.Name, StringComparer.Ordinal))
        {
            string? notEnabled = enabledNames.Contains(plugin.Name) ? null : $"plugin '{plugin.Name}' is not enabled";
            foreach (ModuleDescriptor module in plugin.Modules)
                Add(module, plugin.Name, plugin.FilePath, true, notEnabled);
        }

        return new(project, result, enabled);
    }

    /// <summary>
    /// 프로젝트의 규칙 모듈(ruleModules)을 기술자와 대조하고 이번 빌드에 넣을 모듈만 돌려준다(입력 순서 유지).
    /// targetModules는 타깃이 고른 모듈이다. SDK(엔진) 모듈은 ruleModules에 넣지 않는다.
    /// </summary>
    public IReadOnlyList<NativeModuleInput> Apply(IReadOnlyList<NativeModuleInput> ruleModules, IReadOnlyList<string> targetModules)
    {
        var rules = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput rule in ruleModules)
        {
            if (!modules.TryGetValue(rule.Name, out DescribedModule? described))
                throw new DescriptorException(Project.FilePath,
                    $"module '{rule.Name}' has rules but no descriptor declares it; add it to \"modules\" of " +
                    $"{Path.GetFileName(Project.FilePath)} or of its plugin's .rocksplugin.");

            if (!string.Equals(described.Name, rule.Name, StringComparison.Ordinal))
                throw new DescriptorException(described.OwnerFile,
                    $"module '{described.Name}' differs in case from its rules name '{rule.Name}'.");

            rules.Add(rule.Name);
        }

        foreach (DescribedModule module in Modules)
        {
            if (!rules.Contains(module.Name))
                throw new DescriptorException(module.OwnerFile,
                    $"declared module '{module.Name}' has no rules (expected a ModuleRules class named '{module.Name}' in a *.Build.cs file).");
        }

        void RequireIncluded(string name, string user)
        {
            if (modules.TryGetValue(name, out DescribedModule? module) && !module.Included)
                throw new DescriptorException(module.OwnerFile, $"{user} uses module '{name}', which is excluded: {module.ExcludedReason}.");
        }

        foreach (string target in targetModules)
            RequireIncluded(target, "the target");

        var included = ruleModules.Where(rule => modules[rule.Name].Included).ToList();
        foreach (NativeModuleInput rule in included)
        {
            foreach (string dependency in rule.Dependencies)
                RequireIncluded(dependency, $"module '{rule.Name}'");
        }

        return included;
    }
}
