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
/// Docs/Design/ShaderSystem 9절)가 같은 평가를 쓴다. 사용 예:
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
        return Select(LoadAll(assemblyPath, context), targetName, context.Platform);
    }

    /// <summary>
    /// 규칙 모음에서 targetName 타깃을 고른다. 플랫폼 표(<see cref="ValidateTargetKind"/>)는 고른 타깃에만 한다 — 규칙 모음의 다른 플랫폼 전용
    /// 타깃(예: Android 빌드 중의 Editor 타깃)은 이 빌드를 막지 않는다. 동반 타깃을 더한다(<see cref="WithCompanions"/>).
    /// </summary>
    /// <exception cref="ArgumentException">없는 타깃, 이 플랫폼에서 빌드하지 않는 종류, 모듈이 없는 타깃, 잘못된 동반 타깃.</exception>
    public static LoadedRules Select(LoadedRuleSet set, string targetName, NativePlatform platform)
    {
        var selected = set.Targets.SingleOrDefault(target => target.Name == targetName)
            ?? throw new ArgumentException($"Unknown target '{targetName}'. Available: {string.Join(", ", set.Targets.Select(target => target.Name))}");
        ValidateTargetKind(selected, platform);
        if (selected.Modules.Count == 0)
        {
            throw new ArgumentException("A target must select at least one module.");
        }

        return new(set.Modules, WithCompanions(selected, set.Targets)) { Plugins = set.Plugins };
    }

    /// <summary>
    /// 타깃 종류와 Program 속성, 플랫폼 표를 검사한다(Docs/Design/BuildTargets 3.1절, BS2a). <c>CompileWithEditor</c>·<c>WithEditorData</c>는
    /// Program만 켤 수 있다. 플랫폼은 Game·Client가 Windows·macOS·Android, Editor가 Windows·macOS, Server가 Windows·macOS(Linux는 BS6),
    /// Program은 모두다. 어기면 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static void ValidateTargetKind(TargetRules target, NativePlatform platform)
    {
        ValidateTargetProperties(target);
        if (!SupportsPlatform(target.Type, platform))
        {
            throw new ArgumentException($"Target '{target.Name}': a {target.Type} target is not built for {platform} " +
                "(Docs/Design/BuildTargets section 3.1 platform table).");
        }
    }

    /// <summary>
    /// 타깃 종류와 Program 속성을 검사한다(3.1절 — <c>CompileWithEditor</c>·<c>WithEditorData</c>는 Program만). 규칙 로드가 모든 타깃에
    /// 한다. 어기면 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static void ValidateTargetProperties(TargetRules target)
    {
        if (!Enum.IsDefined(target.Type))
        {
            throw new ArgumentException($"Target '{target.Name}' has an unsupported target type {(int)target.Type}.");
        }

        if (target.Type != TargetType.Program && (target.CompileWithEditor || target.WithEditorData))
        {
            throw new ArgumentException($"Target '{target.Name}' is a {target.Type} target; CompileWithEditor and WithEditorData are Program target " +
                "properties.");
        }
    }

    /// <summary>
    /// 타깃 종류의 플랫폼 표(Docs/Design/BuildTargets 3.1절)다. Game·Client는 Windows·macOS·Android, Editor·Server는 Windows·macOS(Server의
    /// Linux는 BS6), Program은 모두다. Release 배제 정책이 타깃 종류별 평가 플랫폼을 이 표로 고른다.
    /// </summary>
    public static bool SupportsPlatform(TargetType type, NativePlatform platform) => type switch
    {
        TargetType.Game or TargetType.Client => platform is NativePlatform.Windows or NativePlatform.MacOS or NativePlatform.Android,
        TargetType.Editor or TargetType.Server => platform is NativePlatform.Windows or NativePlatform.MacOS,
        _ => true
    };

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

        // 동반 모듈은 주 타깃의 종류·구성으로 type 검사를 받는다(빌드 요청 하나). 출시 타깃(Game·Client·Server)의 Release는 동반 Program의
        // Developer 모듈을 거부하므로 동반은 Program 주 타깃만 둔다(Docs/Design/BuildTargets 3.1절, 실행 파일마다 다른 종류는 후속).
        if (target.Type != TargetType.Program)
        {
            throw new ArgumentException($"Target '{target.Name}' is a {target.Type} target; only Program targets have companion targets.");
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

            // 동반 타깃은 모듈만 이 빌드에 합쳐지므로 그 선행 타깃은 빌드되지 않는다(Docs/Design/BuildTargets 3.11절).
            if (companion.PrerequisiteTargets.Count != 0)
            {
                throw new ArgumentException($"Companion target '{name}' has prerequisite targets; only its modules join the build of '{target.Name}', " +
                    "so list them on that target instead.");
            }

            // 동반 타깃은 주 타깃과 한 빌드로 컴파일되므로 타깃 매크로가 같아야 한다(Docs/Design/BuildTargets 3.1절 — 다른 조합이면 동반
            // 실행 파일이 주 타깃의 매크로로 컴파일된다).
            if (!companion.Kind.Defines.SequenceEqual(target.Kind.Defines))
            {
                throw new ArgumentException($"Companion target '{name}' needs the target macros of '{target.Name}' ({target.Type}: " +
                    $"{string.Join(", ", target.Kind.Defines)}), but has {string.Join(", ", companion.Kind.Defines)}; companions are compiled in the " +
                    "same build.");
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

        public override bool CompileWithEditor => inner.CompileWithEditor;

        public override bool WithEditorData => inner.WithEditorData;

        public override IReadOnlyList<string> TestPrograms => inner.TestPrograms;

        public override IReadOnlyList<string> CompanionTargets => inner.CompanionTargets;

        public override IReadOnlyList<PrerequisiteTarget> PrerequisiteTargets => inner.PrerequisiteTargets;
    }

    /// <summary>
    /// 이 타깃·구성이 선행 타깃을 빌드하는가다(Docs/Design/BuildTargets 3.11절). Developer 모듈을 빼는 구성(Game·Client·Server의 Release —
    /// <see cref="ModuleTypeRules.Allows"/>)은 개발 도구를 빌드하지 않는다.
    /// </summary>
    public static bool BuildsPrerequisites(TargetKind target, NativeConfiguration configuration) =>
        ModuleTypeRules.Allows(target, configuration, Descriptors.ModuleType.Developer);

    /// <summary>
    /// 모든 타깃의 선행 타깃(<see cref="TargetRules.PrerequisiteTargets"/>, Docs/Design/BuildTargets 3.11절)을 검사한다. 규칙 로드가 한다.
    /// </summary>
    /// <exception cref="ArgumentException">빈 이름, 없는 타깃, 자기 자신, 목록 안 중복, Program이 아닌 선행 타깃, 빈 조건 목록·값 중복·정의되지
    /// 않은 값, 이 타깃이 선행 타깃을 빌드하지 않는 구성의 명시, 순환.</exception>
    public static void ValidatePrerequisites(IReadOnlyList<TargetRules> targets)
    {
        var byName = targets.ToDictionary(target => target.Name, StringComparer.Ordinal);
        foreach (TargetRules target in targets)
        {
            HashSet<string> seen = new(StringComparer.Ordinal);
            foreach (PrerequisiteTarget prerequisite in target.PrerequisiteTargets)
            {
                if (string.IsNullOrWhiteSpace(prerequisite.Name))
                {
                    throw new ArgumentException($"Target '{target.Name}' lists a prerequisite target without a name.");
                }

                if (prerequisite.Name == target.Name || !seen.Add(prerequisite.Name))
                {
                    throw new ArgumentException($"Target '{target.Name}' lists prerequisite target '{prerequisite.Name}' twice or itself.");
                }

                if (!byName.TryGetValue(prerequisite.Name, out TargetRules? rules))
                {
                    throw new ArgumentException($"Target '{target.Name}' lists unknown prerequisite target '{prerequisite.Name}'.");
                }

                if (rules.Type != TargetType.Program)
                {
                    throw new ArgumentException($"Prerequisite target '{prerequisite.Name}' of '{target.Name}' is a {rules.Type} target; prerequisite " +
                        "targets are Program targets (they take every module type in every configuration).");
                }

                ValidateCondition(target, prerequisite, "Platforms", prerequisite.Platforms);
                ValidateCondition(target, prerequisite, "Configurations", prerequisite.Configurations);
                NativeConfiguration[] skipped = [.. (prerequisite.Configurations ?? []).Where(configuration => !BuildsPrerequisites(target.Kind, configuration))];
                if (skipped.Length != 0)
                {
                    throw new ArgumentException($"Target '{target.Name}' lists prerequisite target '{prerequisite.Name}' for {skipped[0]}, but a " +
                        $"{skipped[0]} {target.Type} target leaves out Developer modules and builds no prerequisite targets (Docs/Design/BuildTargets " +
                        "section 3.11).");
                }
            }
        }

        // 순환은 선행 타깃 그래프를 깊이 우선으로 훑어 찾는다(조건과 무관하게 — 어느 문맥에서든 순환이 생기지 않게).
        Dictionary<string, bool> finished = new(StringComparer.Ordinal);
        List<string> path = [];
        foreach (TargetRules target in targets)
        {
            VisitPrerequisites(target, byName, finished, path);
        }
    }

    private static void ValidateCondition<T>(TargetRules target, PrerequisiteTarget prerequisite, string field, IReadOnlyList<T>? values)
        where T : struct, Enum
    {
        if (values is null)
        {
            return;
        }

        if (values.Count == 0 || values.Distinct().Count() != values.Count || values.Any(value => !Enum.IsDefined(value)))
        {
            throw new ArgumentException($"Target '{target.Name}': prerequisite target '{prerequisite.Name}' has an empty, duplicated, or unknown " +
                $"{field} condition; leave it null for no condition.");
        }
    }

    // finished[이름]이 false면 지금 경로 위에 있고(다시 만나면 순환), true면 끝까지 훑었다.
    private static void VisitPrerequisites(TargetRules target, IReadOnlyDictionary<string, TargetRules> byName, Dictionary<string, bool> finished,
        List<string> path)
    {
        if (finished.TryGetValue(target.Name, out bool done))
        {
            if (!done)
            {
                int start = path.IndexOf(target.Name);
                throw new ArgumentException($"Prerequisite targets form a cycle: {string.Join(" -> ", path.Skip(start).Append(target.Name))}.");
            }

            return;
        }

        finished[target.Name] = false;
        path.Add(target.Name);
        foreach (PrerequisiteTarget prerequisite in target.PrerequisiteTargets)
        {
            VisitPrerequisites(byName[prerequisite.Name], byName, finished, path);
        }

        path.RemoveAt(path.Count - 1);
        finished[target.Name] = true;
    }

    /// <summary>
    /// target 전에 빌드할 선행 타깃의 규칙이다(Docs/Design/BuildTargets 3.11절). 깊이 우선 후위 순서(선행 타깃의 선행 타깃이 먼저)이고 같은
    /// 타깃은 한 번만 나온다. 각 타깃은 자기 조건 칸(플랫폼·구성)과 자기 종류의 <see cref="BuildsPrerequisites"/>로 거른다. 돌려주는 규칙은
    /// 선행 타깃 자기 것이다(매크로 조합·링크 방식·빌드 정보가 그 타깃 값이다). targets는 <see cref="ValidatePrerequisites"/>를 통과한 규칙
    /// 모음이다.
    /// </summary>
    /// <exception cref="ArgumentException">targets에 없는 선행 타깃.</exception>
    public static IReadOnlyList<TargetRules> PrerequisitesOf(TargetRules target, IReadOnlyList<TargetRules> targets, NativePlatform platform,
        NativeConfiguration configuration)
    {
        var byName = targets.ToDictionary(candidate => candidate.Name, StringComparer.Ordinal);
        List<TargetRules> ordered = [];
        HashSet<string> added = new(StringComparer.Ordinal) { target.Name };
        AddPrerequisites(target, byName, platform, configuration, added, ordered);
        return ordered;
    }

    private static void AddPrerequisites(TargetRules owner, IReadOnlyDictionary<string, TargetRules> byName, NativePlatform platform,
        NativeConfiguration configuration, HashSet<string> added, List<TargetRules> ordered)
    {
        if (!BuildsPrerequisites(owner.Kind, configuration))
        {
            return;
        }

        foreach (PrerequisiteTarget prerequisite in owner.PrerequisiteTargets.Where(value => value.Matches(platform, configuration)))
        {
            TargetRules rules = byName.GetValueOrDefault(prerequisite.Name)
                ?? throw new ArgumentException($"Target '{owner.Name}' lists unknown prerequisite target '{prerequisite.Name}'.");
            // added는 돌려줄 목록의 이름과 주 타깃이다. 검사를 통과한 규칙에는 순환이 없으므로 다시 만나는 이름은 이미 목록에 있다.
            if (!added.Add(rules.Name))
            {
                continue;
            }

            AddPrerequisites(rules, byName, platform, configuration, added, ordered);
            ordered.Add(rules);
        }
    }

    /// <summary>
    /// 규칙 어셈블리의 모든 모듈 규칙을 평가하고 모든 타깃 규칙을 만든다. 규칙은 문맥(플랫폼·구성)마다 다시 평가해야 하므로 호출마다
    /// Configure를 다시 부른다(어셈블리는 한 번만 올라간다). Configure 전에 <see cref="BuildContext.PluginRoot"/>의 플러그인을 찾아
    /// <see cref="BuildContext.Plugins"/>에 넣는다(Docs/Design/BuildTargets 3.9절 BD32 — 켜지 않은 플러그인의 기술자도 검증한다).
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

        // 실행 파일의 플러그인 모듈은 빌드가 켠 플러그인에서 채운다(Docs/Design/BuildTargets 3.9절 BD36). 규칙이 적으면 끈 플러그인도
        // 링크되므로 거부한다.
        if (modules.FirstOrDefault(module => module.PluginModules.Count != 0) is { } listed)
        {
            throw new ArgumentException($"Module rule '{listed.Name}' sets PluginModules; the build fills it from the target's enabled plugins " +
                "(TargetRules.Plugins).");
        }

        foreach (TargetRules target in targets)
        {
            ValidateTargetProperties(target);
        }

        ValidatePrerequisites(targets);

        // type 사이 의존 규칙(Docs/Design/BuildTargets 3.3절 BD25)은 타깃과 무관하게 규칙 로드 때 본다.
        ModuleTypeRules.ValidateDependencies(modules);

        // 공개 헤더 검사 표지는 빌드가 검사 모듈에만 채운다(Docs/Design/BuildTargets 3.5절 BS1b).
        if (modules.FirstOrDefault(module => module.PublicHeaderCheckOf is not null) is { } checker)
        {
            throw new ArgumentException($"Module rule '{checker.Name}' sets PublicHeaderCheckOf; only the build's public header check modules set it.");
        }

        // include 소유 모듈 검사(BS1b)는 모듈 폴더로 소유를 정하므로 서드파티가 아니고 소스나 public include가 있는 모듈은 ModuleDirectory가
        // 있어야 한다.
        string[] homeless = [.. modules.Where(module => !module.ThirdParty && module.ModuleDirectory is null &&
                (module.Sources.Count != 0 || module.PublicIncludeDirectories.Count != 0))
            .Select(module => module.Name).Order(StringComparer.Ordinal)];
        if (homeless.Length != 0)
        {
            throw new ArgumentException($"Module rules {string.Join(", ", homeless)} have no ModuleDirectory; set it to the module folder (relative to " +
                "the root) so the include ownership check can tell which module owns its headers (Docs/Design/BuildTargets section 3.5 BS1b).");
        }

        // 모듈의 플러그인(Docs/Design/ModuleLoader 3.15절)은 빌드가 기술자에서 채운다.
        if (modules.FirstOrDefault(module => module.Plugin is not null) is { } owned)
        {
            throw new ArgumentException($"Module rule '{owned.Name}' sets Plugin; the build fills it from the plugin descriptor that declares the module.");
        }

        // 소스 묶음을 가진 모듈(시험 실행 파일)의 컴파일 환경은 묶음 대상 모듈 환경의 합집합이다(BS1b).
        return new(SourceGroups.WithTargetDependencies(modules), targets) { Plugins = context.Plugins };
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
