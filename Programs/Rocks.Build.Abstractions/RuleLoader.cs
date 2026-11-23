using System.Runtime.Loader;

namespace Rocks.Build.Rules;

/// <summary>
/// 타깃 하나를 고른 규칙 평가 결과다. <see cref="Modules"/>는 모든 모듈 규칙의 Configure 결과이고, <see cref="Plugins"/>는 찾은 모든
/// 플러그인(켜지 않은 것 포함)이다.
/// </summary>
public sealed record LoadedRules(IReadOnlyList<NativeModuleInput> Modules, TargetRules Target)
{
    public IReadOnlyList<Descriptors.PluginDescriptor> Plugins { get; init; } = [];
}

/// <summary>모든 타깃을 담은 규칙 평가 결과다(정책 검사가 타깃마다 도달 집합을 본다). 타깃은 이름 순이다.</summary>
public sealed record LoadedRuleSet(IReadOnlyList<NativeModuleInput> Modules, IReadOnlyList<TargetRules> Targets)
{
    /// <summary>찾은 모든 플러그인(켜지 않은 것 포함, 이름 순)이다.</summary>
    public IReadOnlyList<Descriptors.PluginDescriptor> Plugins { get; init; } = [];
}

/// <summary>
/// 규칙 어셈블리(Rocks.Engine.Rules 또는 게임 규칙)를 읽어 모든 모듈 규칙을 한 문맥으로 평가한다. 빌드 CLI와 정책 검사(Release 배제,
/// Docs/Design/ShaderSystem.ko.md 9절)가 같은 평가를 쓴다. 사용 예:
/// <code>
/// var rules = RuleLoader.Load(rulesPath, "Sandbox", context);          // 빌드
/// var all = RuleLoader.LoadAll(rulesPath, context);                    // 정책 검사
/// </code>
/// </summary>
public static class RuleLoader
{
    /// <summary>
    /// 규칙 어셈블리의 모든 모듈 규칙을 평가하고 targetName 타깃을 고른다.
    /// </summary>
    /// <exception cref="ArgumentException">규칙 생성·평가 실패, 같은 이름의 모듈·타깃, 없는 타깃, 모듈이 없는 타깃.</exception>
    public static LoadedRules Load(string assemblyPath, string targetName, BuildContext context)
    {
        LoadedRuleSet set = LoadAll(assemblyPath, context);
        var selected = set.Targets.SingleOrDefault(target => target.Name == targetName)
            ?? throw new ArgumentException($"Unknown target '{targetName}'. Available: {string.Join(", ", set.Targets.Select(target => target.Name))}");
        if (selected.Modules.Count == 0)
        {
            throw new ArgumentException("A target must select at least one module.");
        }

        return new(set.Modules, WithCompanions(selected, set.Targets)) { Plugins = set.Plugins };
    }

    /// <summary>
    /// 동반 타깃(<see cref="TargetRules.CompanionTargets"/>)의 모듈을 더한 타깃이다. 동반 타깃이 없으면 target 그대로다.
    /// </summary>
    /// <exception cref="ArgumentException">없는 동반 타깃, 자기 자신·중복, Program이 아니거나 Monolithic이 아니거나 플러그인·동반 타깃을 가진 동반
    /// 타깃, Monolithic이 아닌 타깃.</exception>
    public static TargetRules WithCompanions(TargetRules target, IReadOnlyList<TargetRules> targets)
    {
        if (target.CompanionTargets.Count == 0)
        {
            return target;
        }

        if (target.LinkType != NativeLinkType.Monolithic)
        {
            throw new ArgumentException($"Target '{target.Name}' has companion targets but is not Monolithic.");
        }

        List<string> modules = [.. target.Modules];
        HashSet<string> seen = new(StringComparer.Ordinal) { target.Name };
        foreach (string name in target.CompanionTargets)
        {
            if (!seen.Add(name))
            {
                throw new ArgumentException($"Target '{target.Name}' lists companion target '{name}' twice or itself.");
            }

            TargetRules companion = targets.SingleOrDefault(candidate => candidate.Name == name)
                ?? throw new ArgumentException($"Target '{target.Name}' lists unknown companion target '{name}'.");
            if (companion.Type != TargetType.Program || companion.LinkType != NativeLinkType.Monolithic || companion.Plugins.Count != 0 ||
                companion.CompanionTargets.Count != 0)
            {
                throw new ArgumentException($"Companion target '{name}' must be a Monolithic Program target without plugins or companions.");
            }

            modules.AddRange(companion.Modules.Where(module => !modules.Contains(module, StringComparer.OrdinalIgnoreCase)));
        }

        return new CompanionTarget(target, modules);
    }

    // 동반 타깃의 모듈을 더한 타깃이다. 모듈 목록 밖은 원래 타깃 그대로다.
    private sealed class CompanionTarget(TargetRules inner, IReadOnlyList<string> modules) : TargetRules
    {
        public override string Name => inner.Name;

        public override IReadOnlyList<string> Modules => modules;

        public override TargetType Type => inner.Type;

        public override IReadOnlyList<Descriptors.PluginReference> Plugins => inner.Plugins;

        public override NativeLinkType LinkType => inner.LinkType;

        public override LogLevel? LogCompiledMinimum => inner.LogCompiledMinimum;

        public override bool CheckPublicHeaders => inner.CheckPublicHeaders;

        public override string? TestExecutable => inner.TestExecutable;

        public override IReadOnlyList<string> TestPrograms => inner.TestPrograms;

        public override IReadOnlyList<string> CompanionTargets => inner.CompanionTargets;
    }

    /// <summary>
    /// 규칙 어셈블리의 모든 모듈 규칙을 평가하고 모든 타깃 규칙을 만든다. 규칙은 문맥(플랫폼·구성)마다 다시 평가해야 하므로 호출마다
    /// Configure를 다시 부른다(어셈블리는 한 번만 올라간다). Configure 전에 <see cref="BuildContext.PluginRoot"/>의 플러그인을 찾아
    /// <see cref="BuildContext.Plugins"/>에 넣는다(Docs/Design/BuildTargets.ko.md 3.9절 BD32 — 켜지 않은 플러그인의 기술자도 검증한다).
    /// 평가한 모듈 규칙의 문맥은 결과에 실리지 않으므로 호출자가 플러그인 목록이 필요하면 <see cref="LoadedRuleSet.Plugins"/>를 쓴다.
    /// </summary>
    /// <exception cref="ArgumentException">규칙 생성·평가 실패, 같은 이름의 모듈·타깃.</exception>
    /// <exception cref="Descriptors.DescriptorException">플러그인 기술자 오류·중복, 타깃 플러그인 참조의 이름 형식·중복.</exception>
    public static LoadedRuleSet LoadAll(string assemblyPath, BuildContext context)
    {
        context = context with { Plugins = Descriptors.PluginCatalog.Discover([context.PluginRoot]) };
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
        var types = assembly.GetTypes().Where(type => !type.IsAbstract).OrderBy(type => type.FullName, StringComparer.Ordinal).ToArray();
        var modules = types.Where(type => typeof(ModuleRules).IsAssignableFrom(type))
            .Select(type => Configure(type, context)).ToArray();
        var targets = types.Where(type => typeof(TargetRules).IsAssignableFrom(type))
            .Select(type => Create<TargetRules>(type)).OrderBy(target => target.Name, StringComparer.Ordinal).ToArray();
        if (modules.GroupBy(module => module.Name, StringComparer.Ordinal).Any(group => group.Count() != 1) ||
            targets.GroupBy(target => target.Name, StringComparer.Ordinal).Any(group => group.Count() != 1))
        {
            throw new ArgumentException("Duplicate module/target rule name.");
        }

        foreach (TargetRules target in targets)
        {
            Descriptors.TargetPluginReferences.Validate(target.Name, target.Plugins);
        }

        // 실행 파일의 플러그인 모듈은 빌드가 켠 플러그인에서 채운다(Docs/Design/BuildTargets.ko.md 3.9절 BD36). 규칙이 적으면 끈 플러그인도
        // 링크되므로 거부한다.
        if (modules.FirstOrDefault(module => module.PluginModules.Count != 0) is { } listed)
        {
            throw new ArgumentException($"Module rule '{listed.Name}' sets PluginModules; the build fills it from the target's enabled plugins " +
                "(TargetRules.Plugins).");
        }

        // 모듈의 플러그인(Docs/Design/ModuleLoader.ko.md 3.15절)은 빌드가 기술자에서 채운다.
        if (modules.FirstOrDefault(module => module.Plugin is not null) is { } owned)
        {
            throw new ArgumentException($"Module rule '{owned.Name}' sets Plugin; the build fills it from the plugin descriptor that declares the module.");
        }

        return new(modules, targets) { Plugins = context.Plugins };
    }

    private static T Create<T>(Type type)
    {
        try
        {
            return (T)Activator.CreateInstance(type)!;
        }
        catch (Exception error)
        {
            throw new ArgumentException($"Rule '{type.FullName}' could not be constructed: {(error.InnerException ?? error).Message}", error);
        }
    }

    private static NativeModuleInput Configure(Type type, BuildContext context)
    {
        try
        {
            return Create<ModuleRules>(type).Configure(context);
        }
        catch (Exception error)
        {
            throw new ArgumentException($"Module rule '{type.FullName}' failed for {context.Platform}/{context.Configuration}: {(error.InnerException ?? error).Message}", error);
        }
    }
}
