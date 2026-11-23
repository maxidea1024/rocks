using Rocks.Build;
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
            (TargetType.Editor, NativePlatform.MacOS, true), (TargetType.Server, NativePlatform.Windows, true),
            (TargetType.Program, NativePlatform.Android, true)
        ];
        foreach ((TargetType type, NativePlatform platform, bool allowed) in table)
        {
            Expect(!allowed, () => RuleLoader.ValidateTargetKind(new Probe(type), platform), $"{type} on {platform}");
        }

        Require(new Probe(TargetType.Editor).LinkType == NativeLinkType.Modular && new Probe(TargetType.Game).LinkType == NativeLinkType.Monolithic,
            "Editor targets link Modular by default");
        Require(new Probe(TargetType.Program) { Editor = true }.Kind == new TargetKind(TargetType.Program, CompileWithEditor: true), "target kind");
        Console.WriteLine("PASS target kinds: Program properties, platform table, Editor link type");
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

        public override string Name => "Probe";

        public override IReadOnlyList<string> Modules => [];

        public override TargetType Type => type;

        public override bool CompileWithEditor => Editor;

        public override bool WithEditorData => EditorData;
    }
}
