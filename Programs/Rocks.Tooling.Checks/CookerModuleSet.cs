using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 쿠커 모듈 집합 정책이다(Docs/Design/BuildTargets 3.10절 1·4항 BS10, rule <c>cooker-module-set</c>). 쿠커(Commandlet 타깃)는 셰이더 타입을
/// 등록하는 모듈을 출시 타깃과 같게 시작해야 하므로, Commandlet이 빌드되는 호스트(Windows x64·macOS arm64)마다 저장소 규칙을 Release로 평가해
/// 출시 타깃(Game·Client — Server는 셰이더를 쿠킹하지 않는다)의 링크 폐포에 든 엔진 Runtime·ClientOnly 모듈(실행 파일·플러그인 모듈 제외 —
/// 플러그인 몫은 3.10절 2항, ML5b 뒤)이 Commandlet 폐포에 모두 있는지 본다. 사용 예: <c>CookerModuleSet.Check(root, rulesAssembly)</c>.
/// </summary>
public static class CookerModuleSet
{
    /// <summary>쿠커 타깃 이름이다.</summary>
    public const string CookerTarget = "Commandlet";

    private const string Rule = "cooker-module-set";

    // Commandlet이 빌드되는 호스트다.
    private static readonly (NativePlatform Platform, NativeArchitecture Architecture)[] Hosts =
    [
        (NativePlatform.Windows, NativeArchitecture.X64), (NativePlatform.MacOS, NativeArchitecture.Arm64)
    ];

    /// <summary>
    /// 저장소 규칙을 두 호스트의 Release로 평가해 검사한다.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    public static IReadOnlyList<CheckFailure> Check(string root, string rulesAssembly)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        var failures = new List<CheckFailure>();
        foreach ((NativePlatform platform, NativeArchitecture architecture) in Hosts)
        {
            var context = new BuildContext(root, Path.Combine(root, "out", "policy-cooker"), platform, architecture, NativeConfiguration.Release);
            LoadedRuleSet rules = RuleLoader.LoadAll(rulesAssembly, context);
            TargetRules? cooker = rules.Targets.FirstOrDefault(target => target.Name == CookerTarget);
            if (cooker is null)
            {
                failures.Add(new($"target {CookerTarget}", 0, Rule, $"{platform}: the cooker target {CookerTarget} is missing"));
                continue;
            }

            var shipping = new List<(string Name, IReadOnlyList<string> Modules, IReadOnlySet<string> PluginModules)>();
            IReadOnlyList<string>? cookerModules = null;
            foreach (TargetRules target in rules.Targets)
            {
                bool shipped = target.Type is TargetType.Game or TargetType.Client && RuleLoader.SupportsPlatform(target.Type, platform);
                if (!shipped && target != cooker)
                {
                    continue;
                }

                try
                {
                    var set = DescriptorModuleSet.Resolve(DescriptorOwner.ForEngine(target.Name, target.Plugins, root), rules.Plugins, platform,
                        NativeConfiguration.Release, target.Kind);
                    set.Apply(rules.Modules, target.Modules);
                    IReadOnlyList<string> selected = [.. target.Modules, .. set.IncludedPluginModules];
                    if (target == cooker)
                    {
                        cookerModules = selected;
                    }
                    else
                    {
                        shipping.Add((target.Name, selected, set.IncludedPluginModules.ToHashSet(StringComparer.OrdinalIgnoreCase)));
                    }
                }
                catch (DescriptorException error)
                {
                    failures.Add(new($"target {target.Name}", 0, Rule, $"{platform} Release: {error.Message}"));
                }
            }

            if (cookerModules is not null)
            {
                failures.AddRange(Analyze(rules.Modules, cookerModules, shipping, platform));
            }
        }

        return failures;
    }

    /// <summary>
    /// 출시 타깃(이름·고른 모듈·플러그인 모듈)마다 Commandlet 폐포에 없는 엔진 Runtime·ClientOnly 모듈을 실패로 모은다. 규칙 평가와 떨어진 순수
    /// 함수다(음성 대조 시험이 직접 부른다).
    /// </summary>
    /// <exception cref="ArgumentException">타깃이나 모듈이 없는 모듈에 의존한다.</exception>
    public static IReadOnlyList<CheckFailure> Analyze(IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<string> cookerModules,
        IReadOnlyList<(string Name, IReadOnlyList<string> Modules, IReadOnlySet<string> PluginModules)> shippingTargets, NativePlatform platform)
    {
        var byName = modules.ToDictionary(module => module.Name, StringComparer.Ordinal);
        HashSet<string> cooker = LinkClosure.Reachable(cookerModules, byName,
            (name, _) => throw new ArgumentException($"target {CookerTarget}: unknown module {name}"));
        var failures = new List<CheckFailure>();
        foreach ((string target, IReadOnlyList<string> selected, IReadOnlySet<string> pluginModules) in shippingTargets)
        {
            var reached = new SortedSet<string>(LinkClosure.Reachable(selected, byName,
                (name, _) => throw new ArgumentException($"target {target}: unknown module {name}")), StringComparer.Ordinal);
            foreach (string name in reached)
            {
                NativeModuleInput module = byName[name];
                bool engineModule = module.Kind != NativeModuleKind.Executable && !pluginModules.Contains(name) &&
                                    module.Type is ModuleType.Runtime or ModuleType.ClientOnly;
                if (engineModule && !cooker.Contains(name))
                {
                    failures.Add(new($"target {target}", 0, Rule,
                        $"{Rule}: target {target} ({platform} Release) module {name} is not linked into the {CookerTarget}"));
                }
            }
        }

        return failures;
    }
}
