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
}
