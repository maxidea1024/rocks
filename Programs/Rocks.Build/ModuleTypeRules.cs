using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

namespace Rocks.Build;

/// <summary>
/// 모듈 type이 어느 타깃에 들어가는가와 type 사이 의존 규칙이다(Docs/Design/BuildTargets 3.1·3.3절, BD4·BD25, BS2b). 포함 표는
/// <see cref="Allows"/> 한 곳에 있고, 의존 규칙(<see cref="MayDependOn"/>)은 그 표에서 계산한다. 기술자 모듈 거르기
/// (<c>DescriptorModuleSet</c>), 빌드 입력의 도달 검사(<c>InputValidation</c>), 규칙 로드 의존 검사(<c>RuleLoader.LoadAll</c>)가 같은 판정을 쓴다.
/// 사용 예:
/// <code>
/// if (ModuleTypeRules.ExclusionReason(new TargetKind(TargetType.Server), NativeConfiguration.Debug, ModuleType.ClientOnly) is { } reason)
/// {
///     // reason == "ClientOnly modules are not built in Server targets"
/// }
/// </code>
/// </summary>
public static class ModuleTypeRules
{
    // MayDependOn이 훑는 타깃 종류다. Program은 CompileWithEditor를 켠 것과 끈 것 둘이다(WithEditorData는 포함 표에 영향이 없다).
    private static readonly TargetKind[] AllKinds =
    [
        .. Enum.GetValues<TargetType>().Select(type => new TargetKind(type)), new TargetKind(TargetType.Program, CompileWithEditor: true)
    ];

    /// <summary>
    /// type의 모듈이 이 타깃·구성에 들어가는가(3.1절 표)다. Developer는 Game·Client·Server의 Release에서 빠지고, Editor는 Editor 타깃과
    /// <c>CompileWithEditor</c> Program에만, ServerOnly는 Client에서, ClientOnly는 Server에서 빠진다.
    /// </summary>
    public static bool Allows(TargetKind target, NativeConfiguration configuration, ModuleType type) =>
        ExclusionReason(target, configuration, type) is null;

    /// <summary>
    /// type의 모듈이 이 타깃·구성에 들어가지 않는 이유(영어 문장)다. 들어가면 null이다. 기술자 모듈의 빠지는 이유와 빌드 입력 오류가 이
    /// 문장을 쓴다.
    /// </summary>
    public static string? ExclusionReason(TargetKind target, NativeConfiguration configuration, ModuleType type) => type switch
    {
        ModuleType.Runtime => null,
        ModuleType.Developer when configuration == NativeConfiguration.Release && target.Type is TargetType.Game or TargetType.Client or TargetType.Server =>
            $"Developer modules are not built in Release {target.Type} targets",
        ModuleType.Developer => null,
        ModuleType.Editor when target.HasEditor => null,
        ModuleType.Editor when target.Type == TargetType.Program => "Editor modules are not built in Program targets without CompileWithEditor",
        ModuleType.Editor => $"Editor modules are not built in {target.Type} targets",
        ModuleType.ServerOnly when target.Type == TargetType.Client => "ServerOnly modules are not built in Client targets",
        ModuleType.ClientOnly when target.Type == TargetType.Server => "ClientOnly modules are not built in Server targets",
        ModuleType.ServerOnly or ModuleType.ClientOnly => null,
        _ => throw new ArgumentOutOfRangeException(nameof(type), type, "Unknown module type.")
    };

    /// <summary>
    /// type <paramref name="from"/>의 모듈이 type <paramref name="to"/>의 모듈에 의존할 수 있는가(BD25)다. from이 들어가는 모든 (타깃 종류,
    /// Program 속성, 구성)에 to도 들어가야 한다. 그렇지 않으면 from을 넣는 어떤 타깃에서 to가 빠져 빌드가 깨진다.
    /// </summary>
    public static bool MayDependOn(ModuleType from, ModuleType to) =>
        AllKinds.All(kind => Enum.GetValues<NativeConfiguration>().All(configuration =>
            !Allows(kind, configuration, from) || Allows(kind, configuration, to)));

    /// <summary>type <paramref name="from"/>의 모듈이 의존할 수 있는 type 목록이다(오류 문장용, enum 순서).</summary>
    public static IReadOnlyList<ModuleType> DependableTypes(ModuleType from) =>
        [.. Enum.GetValues<ModuleType>().Where(to => MayDependOn(from, to))];

    /// <summary>
    /// type 사이 의존 규칙(BD25)을 검사한다. 실행 파일이 아닌 모듈 A의 두 의존 목록과 실행 중 적재 모듈(<c>DynamicModules</c>)의 모듈 B마다
    /// <see cref="MayDependOn"/>이어야 한다(A를 넣는 어떤 타깃에서 B가 빠지면 빌드가 깨진다). 실행 파일은 한 타깃에만 들어가므로
    /// <see cref="ValidateReach"/>가 대신 본다. modules 밖의 이름은 건너뛴다(빌드 입력 검사가 알린다). 엔진 규칙 로드(모든 규칙 모듈)와 게임
    /// 계획(프로젝트 모듈 — 기술자 type — 과 SDK 모듈)이 부른다. 사용 예: <c>ModuleTypeRules.ValidateDependencies(rules.Modules);</c>
    /// </summary>
    /// <exception cref="ArgumentException">위반이 있다(모든 위반을 한 문장에 담는다).</exception>
    public static void ValidateDependencies(IReadOnlyList<NativeModuleInput> modules)
    {
        var byName = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            byName.TryAdd(module.Name, module);
        }

        var violations = new List<string>();
        foreach (NativeModuleInput module in modules.Where(module => module.Kind != NativeModuleKind.Executable).OrderBy(module => module.Name,
                     StringComparer.Ordinal))
        {
            foreach (string name in module.AllDependencies.Concat(module.DynamicModules).Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (byName.TryGetValue(name, out NativeModuleInput? dependency) && !MayDependOn(module.Type, dependency.Type))
                {
                    violations.Add($"{module.Name} ({module.Type}) -> {dependency.Name} ({dependency.Type})");
                }
            }
        }

        if (violations.Count != 0)
        {
            string allowed = string.Join("; ", Enum.GetValues<ModuleType>().Select(type =>
                $"{type} may depend on {string.Join(", ", DependableTypes(type))}"));
            throw new ArgumentException($"Module type dependency rule violated: {string.Join(", ", violations)}. {allowed} " +
                "(Docs/Design/BuildTargets section 3.3 BD25).");
        }
    }

    /// <summary>
    /// roots(빌드 대상 — 비면 모든 모듈)에서 링크 의존·플러그인 모듈·동적 모듈로 닿는 모듈마다 이 타깃·구성에 들어가는 type인지 검사한다
    /// (3.3절 BS2b). 빌드 입력 검사(<c>InputValidation</c>)와, 메타 생성 전의 계획 단계(엔진·게임 Layout — 이유를 말하는 오류를 다른 실패보다
    /// 먼저 낸다)가 부른다.
    /// </summary>
    /// <exception cref="ArgumentException">닿는 모듈의 type이 타깃·구성에 들어가지 않는다.</exception>
    public static void ValidateReach(TargetKind target, NativeConfiguration configuration, IReadOnlyList<NativeModuleInput> modules,
        IReadOnlyList<string> roots)
    {
        var byName = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            byName.TryAdd(module.Name, module);
        }

        HashSet<string> reached = LinkClosure.Reachable(roots.Count == 0 ? byName.Keys : roots, byName);
        foreach (string name in reached.Order(StringComparer.Ordinal))
        {
            NativeModuleInput module = byName[name];
            if (ExclusionReason(target, configuration, module.Type) is { } reason)
            {
                throw new ArgumentException($"Module {module.Name} has type {module.Type}, but the build reaches it: {reason} " +
                    "(Docs/Design/BuildTargets section 3.3).");
            }
        }
    }
}
