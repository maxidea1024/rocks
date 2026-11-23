using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

namespace Rocks.Build.IntegrationTests;

// 타깃 종류 검사(Docs/Design/BuildTargets 3.1절 BS2a)다: Program 속성은 Program만, 플랫폼 표, Editor 기본 링크 방식, 타깃 Kind.
internal static class TargetKindRulesTests
{
    public static void Run()
    {
        foreach (TargetType type in Enum.GetValues<TargetType>())
        {
            bool program = type == TargetType.Program;
            Expect(!program, () => RuleLoader.ValidateTargetKind(new Probe(type) { Editor = true }, NativePlatform.Windows), "CompileWithEditor on " + type);
            Expect(!program, () => RuleLoader.ValidateTargetKind(new Probe(type) { EditorData = true }, NativePlatform.Windows), "WithEditorData on " + type);
        }

        (TargetType Type, NativePlatform Platform, bool Allowed)[] table =
        [
            (TargetType.Game, NativePlatform.Android, true), (TargetType.Client, NativePlatform.Android, true),
            (TargetType.Editor, NativePlatform.Android, false), (TargetType.Server, NativePlatform.Android, false),
            (TargetType.Editor, NativePlatform.MacOS, true), (TargetType.Editor, NativePlatform.Windows, true),
            (TargetType.Server, NativePlatform.Windows, true), (TargetType.Server, NativePlatform.MacOS, true),
            (TargetType.Client, NativePlatform.Windows, true), (TargetType.Client, NativePlatform.MacOS, true),
            (TargetType.Game, NativePlatform.Windows, true), (TargetType.Game, NativePlatform.MacOS, true),
            (TargetType.Program, NativePlatform.Windows, true), (TargetType.Program, NativePlatform.MacOS, true),
            (TargetType.Program, NativePlatform.Android, true)
        ];
        foreach ((TargetType type, NativePlatform platform, bool allowed) in table)
        {
            Expect(!allowed, () => RuleLoader.ValidateTargetKind(new Probe(type), platform), $"{type} on {platform}");
        }

        Require(new Probe(TargetType.Editor).LinkType == NativeLinkType.Modular && new Probe(TargetType.Game).LinkType == NativeLinkType.Monolithic,
            "Editor targets link Modular by default");
        Require(new Probe(TargetType.Program) { Editor = true }.Kind == new TargetKind(TargetType.Program, CompileWithEditor: true), "target kind");
        // 규칙 로드는 모든 타깃의 Program 속성만 보고, 플랫폼 표는 고른 타깃에만 한다(Load — 다른 플랫폼 전용 타깃이 있어도 이 타깃은 빌드한다).
        RuleLoader.ValidateTargetProperties(new Probe(TargetType.Server));
        LoadedRuleSet set = new([new() { Name = "Probe", Sources = [] }],
            [new Probe(TargetType.Editor) { Named = "Studio", Selected = ["Probe"] }, new Probe(TargetType.Game) { Named = "Game", Selected = ["Probe"] }]);
        Require(RuleLoader.Select(set, "Game", NativePlatform.Android).Target.Name == "Game", "an Android Game build with an Editor target in the rule set");
        Expect(true, () => RuleLoader.Select(set, "Studio", NativePlatform.Android), "an Android Editor build");
        Expect(true, () => RuleLoader.ValidateTargetProperties(new Probe(TargetType.Game) { Editor = true }), "property check on Game");
        Console.WriteLine("PASS target kinds: Program properties, platform table, Editor link type");

        // type 사이 의존 규칙(3.3절 BD25): 의존·동적 모듈 모두 본다. 실행 파일은 빌드 입력의 도달 검사를 받으므로 예외다.
        NativeModuleInput Module(string name, ModuleType type, params string[] dependencies) =>
            new() { Name = name, Sources = [], Type = type, PublicDependencies = dependencies };
        NativeModuleInput[] valid =
        [
            Module("Base", ModuleType.Runtime), Module("Tools", ModuleType.Developer, "Base"), Module("Net", ModuleType.ServerOnly, "Base"),
            Module("Render", ModuleType.ClientOnly, "Base"), Module("Studio", ModuleType.Editor, "Tools", "Net", "Render"),
            new() { Name = "Tests", Kind = NativeModuleKind.Executable, Sources = [], PrivateDependencies = ["Tools", "Studio"] }
        ];
        ModuleTypeRules.ValidateDependencies(valid);
        foreach ((string name, NativeModuleInput[] modules, string expected) in new (string, NativeModuleInput[], string)[]
                 {
                     ("Runtime -> Developer", [.. valid, Module("Game", ModuleType.Runtime, "Tools")], "Game (Runtime) -> Tools (Developer)"),
                     ("Runtime -> ClientOnly", [.. valid, Module("Game", ModuleType.Runtime) with { PrivateDependencies = ["Render"] }],
                         "Game (Runtime) -> Render (ClientOnly)"),
                     ("ServerOnly -> ClientOnly", [.. valid, Module("Auth", ModuleType.ServerOnly, "Render")], "Auth (ServerOnly) -> Render (ClientOnly)"),
                     ("Developer -> Editor (dynamic)", [.. valid, Module("Cook", ModuleType.Developer) with { DynamicModules = ["Studio"] }],
                         "Cook (Developer) -> Studio (Editor)"),
                     ("ClientOnly -> Developer", [.. valid, Module("Hud", ModuleType.ClientOnly, "Tools")], "Hud (ClientOnly) -> Tools (Developer)")
                 })
        {
            try
            {
                ModuleTypeRules.ValidateDependencies(modules);
                Require(false, name + " was accepted");
            }
            catch (ArgumentException error)
            {
                Require(error.Message.Contains(expected, StringComparison.Ordinal) &&
                    error.Message.Contains("Runtime may depend on Runtime;", StringComparison.Ordinal), name + ": " + error.Message);
            }
        }

        Console.WriteLine("PASS module types: dependency rule at rule load (dependencies, dynamic modules, executables exempt)");
    }

    private static void Expect(bool rejected, Action action, string name)
    {
        try
        {
            action();
        }
        catch (ArgumentException)
        {
            Require(rejected, name + " was rejected");
            return;
        }

        Require(!rejected, name + " was not rejected");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class Probe(TargetType type) : TargetRules
    {
        public bool Editor { get; init; }

        public bool EditorData { get; init; }

        public string Named { get; init; } = "Probe";

        public IReadOnlyList<string> Selected { get; init; } = [];

        public override string Name => Named;

        public override IReadOnlyList<string> Modules => Selected;

        public override TargetType Type => type;

        public override bool CompileWithEditor => Editor;

        public override bool WithEditorData => EditorData;
    }
}
