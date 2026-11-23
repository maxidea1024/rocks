using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

/// <summary>
/// 모듈 type(Docs/Design/BuildTargets 3.1·3.3절 BS2b)이다: 포함 표(다섯 종류 × Program 속성 × 구성), 표에서 계산한 의존 규칙이 3.3절 의존
/// 표와 같음, 빌드 입력의 도달 검사(엔진·SDK 모듈).
/// </summary>
internal static class ModuleTypeTests
{
    public static void Run(NativeBuildRequest request, Action<string> pass)
    {
        // 포함 표: 행은 타깃 종류(Program은 속성 둘), 열은 Runtime·Developer·Editor·ServerOnly·ClientOnly, 값은 Debug/Release 포함 여부다.
        (TargetKind Kind, string Expected)[] table =
        [
            (new(TargetType.Game), "11 10 00 11 11"),
            (new(TargetType.Client), "11 10 00 00 11"),
            (new(TargetType.Server), "11 10 00 11 00"),
            (new(TargetType.Editor), "11 11 11 11 11"),
            (new(TargetType.Program), "11 11 00 11 11"),
            (new(TargetType.Program, CompileWithEditor: true), "11 11 11 11 11"),
            (new(TargetType.Program, WithEditorData: true), "11 11 00 11 11")
        ];
        foreach ((TargetKind kind, string expected) in table)
        {
            string row = string.Join(" ", Enum.GetValues<ModuleType>().Select(type =>
                Bit(ModuleTypeRules.Allows(kind, NativeConfiguration.Debug, type)) + Bit(ModuleTypeRules.Allows(kind, NativeConfiguration.Release, type))));
            Check(row == expected, $"{kind}: inclusion row {row}, expected {expected}");
            // Development·Asan·Coverage는 Debug와 같다(Developer가 빠지는 것은 Release뿐이다).
            foreach (NativeConfiguration configuration in new[] { NativeConfiguration.Development, NativeConfiguration.Asan, NativeConfiguration.Coverage })
            {
                Check(Enum.GetValues<ModuleType>().All(type => ModuleTypeRules.Allows(kind, configuration, type) ==
                    ModuleTypeRules.Allows(kind, NativeConfiguration.Debug, type)), $"{kind} {configuration} differs from Debug");
            }
        }

        Check(ModuleTypeRules.ExclusionReason(new(TargetType.Server), NativeConfiguration.Debug, ModuleType.ClientOnly) ==
            "ClientOnly modules are not built in Server targets" &&
            ModuleTypeRules.ExclusionReason(new(TargetType.Client), NativeConfiguration.Debug, ModuleType.ServerOnly) ==
            "ServerOnly modules are not built in Client targets" &&
            ModuleTypeRules.ExclusionReason(new(TargetType.Game), NativeConfiguration.Release, ModuleType.Developer) ==
            "Developer modules are not built in Release Game targets" &&
            ModuleTypeRules.ExclusionReason(new(TargetType.Program), NativeConfiguration.Debug, ModuleType.Editor) ==
            "Editor modules are not built in Program targets without CompileWithEditor" &&
            ModuleTypeRules.ExclusionReason(new(TargetType.Game), NativeConfiguration.Debug, ModuleType.Editor) ==
            "Editor modules are not built in Game targets", "exclusion reasons");
        pass("module types: inclusion table for the five target types, Program properties and configurations");

        // 의존 표(3.3절, BD25)를 포함 표에서 계산한 결과와 대조한다.
        var dependable = new Dictionary<ModuleType, ModuleType[]>
        {
            [ModuleType.Runtime] = [ModuleType.Runtime],
            [ModuleType.Developer] = [ModuleType.Runtime, ModuleType.Developer],
            [ModuleType.Editor] = Enum.GetValues<ModuleType>(),
            [ModuleType.ServerOnly] = [ModuleType.Runtime, ModuleType.ServerOnly],
            [ModuleType.ClientOnly] = [ModuleType.Runtime, ModuleType.ClientOnly]
        };
        foreach ((ModuleType from, ModuleType[] expected) in dependable)
        {
            Check(ModuleTypeRules.DependableTypes(from).SequenceEqual(expected),
                $"{from} may depend on {string.Join(", ", ModuleTypeRules.DependableTypes(from))}, expected {string.Join(", ", expected)}");
        }

        pass("module types: the dependency rule computed from the inclusion table matches the 3.3 table");

        // 빌드 입력의 도달 검사: 빌드 대상에서 닿는 모듈만 본다(링크 의존·동적 모듈). 닿지 않는 모듈은 type이 맞지 않아도 된다.
        NativeModuleInput Module(string name, ModuleType type, params string[] dependencies) =>
            new() { Name = name, Sources = [], Type = type, PrivateDependencies = dependencies };
        NativeModuleInput app = new() { Name = "App", Kind = NativeModuleKind.Executable, Sources = [], PrivateDependencies = ["Uses"] };
        void Reach(TargetKind kind, NativeConfiguration configuration, ModuleType type, string? expected, bool dynamic = false)
        {
            NativeModuleInput user = dynamic ? Module("Uses", ModuleType.Runtime) with { DynamicModules = ["Reached"] } : Module("Uses", ModuleType.Runtime, "Reached");
            NativeBuildRequest probe = request with
            {
                Target = kind,
                Configuration = configuration,
                Targets = ["App"],
                Modules = [app, user, Module("Reached", type), Module("Unreached", ModuleType.Editor)]
            };
            try
            {
                InputValidation.Validate(probe);
                Check(expected is null, $"{kind} {configuration} reaching a {type} module was accepted");
            }
            catch (ArgumentException error) when (expected is not null)
            {
                Check(error.Message.Contains("Module Reached has type " + type, StringComparison.Ordinal) &&
                    error.Message.Contains(expected, StringComparison.Ordinal), error.Message);
            }
        }

        Reach(new(TargetType.Game), NativeConfiguration.Release, ModuleType.Developer, "Developer modules are not built in Release Game targets");
        Reach(new(TargetType.Game), NativeConfiguration.Debug, ModuleType.Developer, null);
        Reach(new(TargetType.Client), NativeConfiguration.Debug, ModuleType.ServerOnly, "ServerOnly modules are not built in Client targets");
        Reach(new(TargetType.Server), NativeConfiguration.Debug, ModuleType.ClientOnly, "ClientOnly modules are not built in Server targets", dynamic: true);
        Reach(new(TargetType.Program), NativeConfiguration.Debug, ModuleType.Editor, "without CompileWithEditor");
        Reach(new(TargetType.Program, CompileWithEditor: true), NativeConfiguration.Release, ModuleType.Editor, null);
        Reach(new(TargetType.Program), NativeConfiguration.Release, ModuleType.Developer, null);
        pass("module types: the build input rejects reached modules of a type the target and configuration do not include");
    }

    private static string Bit(bool value) => value ? "1" : "0";

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
