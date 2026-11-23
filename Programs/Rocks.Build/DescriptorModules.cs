using Rocks.Build.Rules;

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
/// 기술자 모듈 집합을 정하는 빌드의 주인이다(Docs/Design/BuildTargets 3.9절). 설치 SDK 게임 빌드는 프로젝트 기술자(프로젝트 모듈,
/// 프로젝트 참조와 타깃 참조를 합친 플러그인 참조, targetPlatforms)이고, 엔진 저장소 빌드는 프로젝트가 없이 타깃 규칙의 플러그인 참조만
/// 가진다. 엔진 저장소의 Core·Engine 모듈은 기술자가 없으므로 대조하지 않는다.
/// </summary>
public sealed record DescriptorOwner(ProjectDescriptor? Project, IReadOnlyList<PluginReference> Plugins, PluginReferrer Referrer)
{
    /// <summary>엔진 저장소 루트다(엔진 타깃만). 플러그인 모듈 폴더·계층 검사가 규칙의 ModuleDirectory(루트 기준)와 대조할 때 쓴다.</summary>
    public string? EngineRoot { get; init; }

    /// <summary>
    /// 프로젝트와 그 타깃이다. 타깃 참조가 같은 이름의 프로젝트 참조를 이긴다(<see cref="TargetPluginReferences.Combine"/>).
    /// </summary>
    public static DescriptorOwner ForProject(ProjectDescriptor project, string targetName, IReadOnlyList<PluginReference> targetPlugins)
    {
        TargetPluginReferences.Validate(targetName, targetPlugins);
        PluginReferrer referrer = targetPlugins.Count == 0
            ? PluginReferrer.Of(project)
            : new($"target '{targetName}' or project", project.FilePath);
        return new(project, TargetPluginReferences.Combine(targetPlugins, project.Plugins), referrer);
    }

    /// <summary>엔진 저장소(rootDirectory) 타깃이다(프로젝트 없음).</summary>
    public static DescriptorOwner ForEngine(string targetName, IReadOnlyList<PluginReference> targetPlugins, string rootDirectory)
    {
        TargetPluginReferences.Validate(targetName, targetPlugins);
        return new(null, targetPlugins, PluginReferrer.OfTarget(targetName)) { EngineRoot = Path.GetFullPath(rootDirectory) };
    }
}

/// <summary>
/// 기술자로 이번 빌드(타깃 종류·플랫폼·구성)에 넣을 모듈을 정하고, C# 모듈 규칙과 서로 맞는지 검사한다(UE
/// UnrealBuildTool이 .uproject·.uplugin의 Modules 목록으로 빌드할 모듈을 고르는 것과 같은 역할).
///
/// 사용 예(게임 빌드):
/// <code>
/// var project = DescriptorReader.ReadProject(projectFile);
/// var plugins = PluginCatalog.Discover([Path.Combine(project.RootDirectory, "Plugins")]);
/// var owner = DescriptorOwner.ForProject(project, target.Name, target.Plugins);
/// var set = DescriptorModuleSet.Resolve(owner, plugins, platform, configuration, target.Type);
/// var modules = set.Apply(ruleModules, target.Modules);   // 빌드할 규칙 모듈만 남는다
/// </code>
///
/// 켤 플러그인: owner의 참조와, Game 타깃이면 enabledByDefault 플러그인(Program 타깃은 명시한 것만 — BD34), 그리고 그 의존 플러그인.
///
/// 모듈을 빼는 규칙(BD35):
/// <list type="bullet">
/// <item>켜지지 않은 플러그인의 모듈(PluginCatalog.ResolveEnabled 기준).</item>
/// <item>Editor 모듈: 아직 에디터 타깃이 없으므로 항상 뺀다.</item>
/// <item>Developer 모듈: Game 타깃의 Release(배포)에서 뺀다. UE 5.8의 DeveloperTool(<c>bBuildDeveloperTools</c> 기본은 에디터·프로그램
/// 타깃이거나 Test·Shipping이 아닌 구성 — Rocks에는 Test 구성이 없다)과 같은 의미다. Program 타깃(명령행 도구·시험)은 Release에서도 넣는다.
/// UE의 Developer(에디터·프로그램 타깃 전용)와는 다르다.</item>
/// <item>platforms에 이번 플랫폼이 없는 모듈.</item>
/// </list>
/// 오류(DescriptorException): 프로젝트가 이번 플랫폼을 targetPlatforms에 두지 않음, 여러 기술자가 같은 모듈 이름을
/// 선언, 규칙은 있는데 기술자가 없는 모듈(게임 빌드), 기술자는 있는데 규칙이 없는 모듈, 빠진 모듈을 타깃이나 남은 모듈이 참조(링크
/// 의존·DynamicModules).
///
/// 규칙 계약: 규칙 로더는 이름을 알기 위해 모든 모듈 규칙의 Configure를 먼저 실행한 뒤 이 필터를 적용한다.
/// 따라서 규칙은 빠질 모듈(켜지지 않은 플러그인, 다른 플랫폼 전용)이라도 예외 없이 NativeModuleInput을
/// 돌려줘야 한다(소스 폴더를 플랫폼 조건으로 만들지 않는다).
/// </summary>
public sealed class DescriptorModuleSet
{
    private readonly Dictionary<string, DescribedModule> modules;

    private readonly Dictionary<string, PluginDescriptor> plugins;

    private DescriptorModuleSet(DescriptorOwner owner, Dictionary<string, DescribedModule> modules,
        IReadOnlyList<PluginDescriptor> enabledPlugins, IReadOnlyList<PluginDescriptor> available)
    {
        Owner = owner;
        this.modules = modules;
        EnabledPlugins = enabledPlugins;
        plugins = available.ToDictionary(plugin => plugin.Name, StringComparer.Ordinal);
    }

    public DescriptorOwner Owner { get; }

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
    /// owner와 찾은 플러그인(available)으로 targetType 타깃의 모듈 집합을 정한다. available에는 켜지지 않은 플러그인도 넣는다(그
    /// 모듈도 규칙과 대조하고, 참조하면 "켜지지 않음"으로 알려 준다).
    /// </summary>
    public static DescriptorModuleSet Resolve(DescriptorOwner owner, IReadOnlyList<PluginDescriptor> available,
        NativePlatform platform, NativeConfiguration configuration, TargetType targetType)
    {
        ProjectDescriptor? project = owner.Project;
        if (project is not null && !project.TargetPlatforms.Contains(platform))
        {
            throw new DescriptorException(project.FilePath,
                $"the project does not target {platform} (targetPlatforms: {string.Join(", ", project.TargetPlatforms)}).");
        }

        IReadOnlyList<PluginDescriptor> enabled = PluginCatalog.ResolveEnabled(owner.Plugins, owner.Referrer, available,
            applyDefaults: targetType == TargetType.Game);
        var enabledNames = enabled.Select(plugin => plugin.Name).ToHashSet(StringComparer.Ordinal);
        var result = new Dictionary<string, DescribedModule>(StringComparer.OrdinalIgnoreCase);

        void Add(ModuleDescriptor module, string ownerName, string file, bool fromPlugin, string? pluginNotEnabled)
        {
            string? reason = pluginNotEnabled ?? ExclusionReason(module, platform, configuration, targetType);
            if (!result.TryAdd(module.Name, new(module, ownerName, file, fromPlugin, reason)))
            {
                throw new DescriptorException(file,
                    $"module '{module.Name}' is also declared by {result[module.Name].OwnerFile}.");
            }
        }

        if (project is not null)
        {
            foreach (ModuleDescriptor module in project.Modules)
            {
                Add(module, project.Name, project.FilePath, false, null);
            }
        }

        foreach (PluginDescriptor plugin in available.OrderBy(plugin => plugin.Name, StringComparer.Ordinal))
        {
            string? notEnabled = enabledNames.Contains(plugin.Name) ? null : $"plugin '{plugin.Name}' is not enabled";
            foreach (ModuleDescriptor module in plugin.Modules)
            {
                Add(module, plugin.Name, plugin.FilePath, true, notEnabled);
            }
        }

        return new(owner, result, enabled, available);
    }

    /// <summary>
    /// 켠 기술자의 모듈 하나를 이번 문맥에서 빼는 이유다(BD35 표). 들어가면 null이다.
    /// </summary>
    public static string? ExclusionReason(ModuleDescriptor module, NativePlatform platform, NativeConfiguration configuration,
        TargetType targetType) => module.Type switch
        {
            ModuleType.Editor => "Editor modules are not built yet (there is no editor target)",
            ModuleType.Developer when targetType == TargetType.Game && configuration == NativeConfiguration.Release =>
                "Developer modules are not built in Release game targets",
            _ => module.SupportsPlatform(platform) ? null : $"the module does not support {platform}"
        };

    /// <summary>
    /// 규칙 모듈(ruleModules)을 기술자와 대조하고 이번 빌드에 넣을 모듈만 돌려준다(입력 순서 유지).
    /// targetModules는 타깃이 고른 모듈이다. 게임 빌드는 프로젝트 규칙 모듈만 넘기며(SDK 엔진 모듈은 넣지 않는다) 모든 규칙 모듈에 기술자가
    /// 있어야 한다. 엔진 저장소 빌드(owner에 프로젝트가 없음)는 모든 규칙 모듈을 넘기고, 기술자가 선언하지 않은 모듈(Core·Engine 모듈)은
    /// 그대로 통과시키며, 결과는 타깃 모듈과 켠 플러그인 모듈에서 닿는 모듈로 줄인다(빠진 모듈 사용 검사도 그 범위다).
    /// </summary>
    public IReadOnlyList<NativeModuleInput> Apply(IReadOnlyList<NativeModuleInput> ruleModules, IReadOnlyList<string> targetModules)
    {
        var rules = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput rule in ruleModules)
        {
            if (!modules.TryGetValue(rule.Name, out DescribedModule? described))
            {
                if (Owner.Project is null)
                {
                    continue;
                }

                throw new DescriptorException(Owner.Project.FilePath,
                    $"module '{rule.Name}' has rules but no descriptor declares it; add it to \"modules\" of " +
                    $"{Path.GetFileName(Owner.Project.FilePath)} or of its plugin's .rocksplugin.");
            }

            if (!string.Equals(described.Name, rule.Name, StringComparison.Ordinal))
            {
                throw new DescriptorException(described.OwnerFile,
                    $"module '{described.Name}' differs in case from its rules name '{rule.Name}'.");
            }

            rules.Add(rule.Name);
        }

        foreach (DescribedModule module in Modules)
        {
            if (!rules.Contains(module.Name))
            {
                throw new DescriptorException(module.OwnerFile,
                    $"declared module '{module.Name}' has no rules (expected a ModuleRules class named '{module.Name}' in a *.Build.cs file).");
            }
        }

        ValidateLayers(ruleModules);

        void RequireIncluded(string name, string user)
        {
            if (modules.TryGetValue(name, out DescribedModule? module) && !module.Included)
            {
                throw new DescriptorException(module.OwnerFile, $"{user} uses module '{name}', which is excluded: {module.ExcludedReason}.");
            }
        }

        foreach (string target in targetModules)
        {
            RequireIncluded(target, "the target");
        }

        var included = ruleModules.Where(rule => !modules.TryGetValue(rule.Name, out DescribedModule? module) || module.Included).ToList();
        if (Owner.Project is null)
        {
            // 엔진 저장소 규칙 어셈블리는 모든 타깃의 모듈을 담는다. 이 타깃이 빌드하는 모듈(타깃 모듈과 켠 플러그인 모듈에서 닿는 모듈)만
            // 남겨, 다른 타깃이 켠 플러그인에 의존하는 모듈(예: 플러그인 시험을 모으는 EngineTests)이 이 타깃의 해석을 막지 않게 한다.
            var byName = included.ToDictionary(rule => rule.Name, StringComparer.OrdinalIgnoreCase);
            HashSet<string> reached = LinkClosure.Reachable(targetModules.Concat(IncludedPluginModules), byName);
            included = included.Where(rule => reached.Contains(rule.Name)).ToList();
        }

        foreach (NativeModuleInput rule in included)
        {
            // 실행 중 적재 모듈도 같다(Monolithic이 링크한다 — Docs/Design/ModuleLoader.ko.md 3.8절, BuildTargets BD25).
            foreach (string dependency in rule.AllDependencies.Concat(rule.DynamicModules))
            {
                RequireIncluded(dependency, $"module '{rule.Name}'");
            }
        }

        // 기술자의 loadingPhase를 모듈 표 행으로 넘긴다(Docs/Design/ModuleLoader.ko.md 3.2절). 기술자가 없는 엔진 모듈은 규칙 값 그대로다.
        // 플러그인 모듈에는 플러그인 이름과 플러그인 루트 기준 폴더를 넣는다(Docs/Design/ModuleLoader.ko.md 3.15절 — Modular 공유 라이브러리 폴더).
        return included.Select(rule => modules.TryGetValue(rule.Name, out DescribedModule? module)
            ? rule with { LoadingPhase = module.Descriptor.LoadingPhase, Plugin = module.FromPlugin ? PluginOf(module) : null }
            : rule).ToList();
    }

    // 플러그인 모듈의 플러그인 이름과 플러그인 루트(엔진 저장소 Engine/Plugins, 프로젝트 Plugins) 기준 폴더(/ 구분)다.
    private ModulePlugin PluginOf(DescribedModule module)
    {
        string root = Owner.Project is { } project ? Path.Combine(project.RootDirectory, "Plugins")
            : Path.Combine(Owner.EngineRoot ?? throw new InvalidOperationException("Internal error: an engine descriptor owner has no root."),
                "Engine", "Plugins");
        string directory = Path.GetRelativePath(Path.GetFullPath(root), Path.GetDirectoryName(Path.GetFullPath(module.OwnerFile))!)
            .Replace(Path.DirectorySeparatorChar, '/');
        return new(module.OwnerName, directory);
    }

    /// <summary>
    /// 켠 플러그인의 모듈(<see cref="IncludedPluginModules"/>)을 타깃의 주 실행 파일(targetModules[0])에 링크하도록 표시한다(Docs/Design/
    /// Docs/Design/BuildTargets 3.9절 BD36, UE가 켠 플러그인 모듈을 LaunchModuleName의 주 binary에 넣는 것과 같다). 실행 파일이 직접 의존하지
    /// 않아도 링크되고 모듈 표에 기술자 단계의 보통 행으로 실린다. 부 실행 파일에는 넣지 않는다. 주 모듈이 실행 파일이 아닌 타깃(엔진
    /// 라이브러리 묶음 Engine)은 그대로 둔다(빌드 대상에만 더한다). modules는 <see cref="Apply"/>의 결과(게임 빌드는 SDK 모듈을 더한 것)다.
    /// </summary>
    public IReadOnlyList<NativeModuleInput> LinkIntoPrimary(IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<string> targetModules)
    {
        IReadOnlyList<string> plugins = IncludedPluginModules;
        if (plugins.Count == 0 || targetModules.Count == 0)
        {
            return modules;
        }

        string primary = targetModules[0];
        return modules.Select(module => string.Equals(module.Name, primary, StringComparison.OrdinalIgnoreCase) &&
            module.Kind == NativeModuleKind.Executable
                ? module with { PluginModules = plugins }
                : module).ToList();
    }

    /// <summary>
    /// 플러그인 모듈의 위치와 계층 의존을 검사한다(Docs/Design/BuildTargets 3.9절 BD37·BD39). 타깃과 무관한 구조 규칙이라 모든 규칙
    /// 모듈(빠진 모듈 포함)을 본다. 의존은 링크 의존과 실행 중 적재(DynamicModules) 둘 다다.
    /// <list type="bullet">
    /// <item>엔진 저장소: 플러그인 모듈의 ModuleDirectory는 &lt;플러그인&gt;/Source/&lt;모듈&gt;이다(PluginModuleRules).</item>
    /// <item>엔진 저장소: Core·Engine/Source 모듈은 플러그인 모듈에 의존하지 않는다(UE scope 검사와 같다).</item>
    /// <item>플러그인 모듈이 다른 플러그인의 모듈에 의존하면 그 플러그인을 자기 .rocksplugin plugins에 적어야 한다.</item>
    /// </list>
    /// </summary>
    private void ValidateLayers(IReadOnlyList<NativeModuleInput> ruleModules)
    {
        DescribedModule? PluginModule(string name) =>
            modules.TryGetValue(name, out DescribedModule? module) && module.FromPlugin ? module : null;

        foreach (NativeModuleInput rule in ruleModules)
        {
            DescribedModule? self = PluginModule(rule.Name);
            if (Owner.EngineRoot is string root && self is not null)
            {
                string expected = Relative(root, Path.Combine(Path.GetDirectoryName(self.OwnerFile)!, "Source", rule.Name));
                if (!string.Equals(rule.ModuleDirectory?.Replace('\\', '/'), expected, StringComparison.Ordinal))
                {
                    throw new DescriptorException(self.OwnerFile,
                        $"plugin module '{rule.Name}' must live in {expected} (its rules give '{rule.ModuleDirectory}'); derive its rules from PluginModuleRules.");
                }
            }

            bool engineModule = Owner.EngineRoot is not null && self is null && IsEngineLayer(rule.ModuleDirectory);
            foreach (string dependency in rule.AllDependencies.Concat(rule.DynamicModules))
            {
                if (PluginModule(dependency) is not DescribedModule target)
                {
                    continue;
                }

                if (engineModule)
                {
                    throw new DescriptorException(target.OwnerFile,
                        $"engine module '{rule.Name}' must not depend on plugin module '{dependency}' of plugin '{target.OwnerName}'; " +
                        "Core and Engine/Source modules never reference plugins.");
                }

                if (self is not null && target.OwnerName != self.OwnerName &&
                    !plugins[self.OwnerName].Plugins.Any(reference => reference.Name == target.OwnerName))
                {
                    throw new DescriptorException(self.OwnerFile,
                        $"plugin module '{rule.Name}' depends on module '{dependency}' of plugin '{target.OwnerName}'; add " +
                        $"{{ \"name\": \"{target.OwnerName}\", \"enabled\": true }} to \"plugins\" of {Path.GetFileName(self.OwnerFile)}.");
                }
            }
        }
    }

    // Core·Engine/Source 아래 모듈(엔진 계층)인가. Engine/Plugins와 Programs는 아니다.
    private static bool IsEngineLayer(string? moduleDirectory)
    {
        string? path = moduleDirectory?.Replace('\\', '/');
        return path is not null && (path.StartsWith("Core/", StringComparison.Ordinal) || path.StartsWith("Engine/Source/", StringComparison.Ordinal));
    }

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
}
