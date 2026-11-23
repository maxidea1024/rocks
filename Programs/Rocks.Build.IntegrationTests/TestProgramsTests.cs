using Rocks.Build.Cli;
using Rocks.Build.Rules;

namespace Rocks.Build.IntegrationTests;

// 시험 동반 타깃(TargetRules.TestPrograms — Docs/Design/BuildTargets.ko.md 3.8절 BS8d6)의 검사다. test 명령이 하니스 전에 빌드할
// (타깃, 실행 파일 모듈)을 고르고, 동반 타깃이 Program이 아니거나 시험 실행 파일·동반 타깃을 가지거나 실행 파일 모듈 하나가 아니거나
// 중복이면 거부한다. --output을 준 test의 동반 출력은 형제 디렉터리다.
internal static class TestProgramsTests
{
    public static void Run()
    {
        NativeModuleInput[] modules =
        [
            new() { Name = "Tests", Kind = NativeModuleKind.Executable },
            new() { Name = "Probe", Kind = NativeModuleKind.Executable },
            new() { Name = "Library" }
        ];
        var targets = new Dictionary<string, TargetRules>(StringComparer.Ordinal)
        {
            ["ProbeTarget"] = new Probe("ProbeTarget", ["Probe"]),
            ["GameTarget"] = new Probe("GameTarget", ["Probe"]) { Kind = TargetType.Game },
            ["WithTests"] = new Probe("WithTests", ["Probe"]) { Tests = "Probe" },
            ["WithPrograms"] = new Probe("WithPrograms", ["Probe"]) { Programs = ["ProbeTarget"] },
            ["LibraryOnly"] = new Probe("LibraryOnly", ["Library"]),
            ["TwoModules"] = new Probe("TwoModules", ["Probe", "Tests"])
        };
        TargetRules Load(string name) => targets.TryGetValue(name, out TargetRules? target) ? target
            : throw new ArgumentException($"Target {name} has no rules.");
        LoadedRules Runner(params string[] programs) => new(modules, new Probe("Runner", ["Tests"]) { Tests = "Tests", Programs = programs });

        Require(BuildCommand.TestPrograms(Runner(), Load).Count == 0, "a target without test programs");
        var found = BuildCommand.TestPrograms(Runner("ProbeTarget"), Load);
        Require(found.Count == 1 && found[0] == ("ProbeTarget", "Probe"), "test program target and executable: " + string.Join(", ", found));
        Expect("listed more than once", () => BuildCommand.TestPrograms(Runner("ProbeTarget", "ProbeTarget"), Load));
        // 이름은 대소문자까지 구별한다(다른 철자는 다른 타깃 이름이라 규칙이 없다).
        Expect("has no rules", () => BuildCommand.TestPrograms(Runner("ProbeTarget", "probetarget"), Load));
        foreach (string invalid in new[] { "GameTarget", "WithTests", "WithPrograms", "LibraryOnly", "TwoModules" })
        {
            Expect("must be a Program target with one executable", () => BuildCommand.TestPrograms(Runner(invalid), Load));
        }

        Expect("has no rules", () => BuildCommand.TestPrograms(Runner("Missing"), Load));

        string root = Path.GetFullPath("rocks-root");
        Require(BuildCommand.TestProgramOutput(null, root, "windows", "LaunchProbeModular", NativeConfiguration.Debug) ==
            EnginePlanning.DefaultOutput(root, "windows", "LaunchProbeModular", NativeConfiguration.Debug), "default companion output");
        string custom = Path.Combine(root, "out", "ci-modular");
        Require(BuildCommand.TestProgramOutput(custom, root, "windows", "LaunchProbeModular", NativeConfiguration.Debug) ==
            Path.Combine(root, "out", "ci-modular-launchprobemodular"), "companion output next to a custom test output");
        Console.WriteLine("PASS test programs: selection, duplicates, invalid companion targets, companion output");
    }

    private static void Expect(string message, Action action)
    {
        try
        {
            action();
        }
        catch (ArgumentException error) when (error.Message.Contains(message, StringComparison.Ordinal))
        {
            return;
        }

        throw new InvalidOperationException("Expected test program rejection: " + message);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class Probe(string name, IReadOnlyList<string> modules) : TargetRules
    {
        public TargetType Kind { get; init; } = TargetType.Program;

        public string? Tests { get; init; }

        public IReadOnlyList<string> Programs { get; init; } = [];

        public override string Name => name;

        public override IReadOnlyList<string> Modules => modules;

        public override TargetType Type => Kind;

        public override string? TestExecutable => Tests;

        public override IReadOnlyList<string> TestPrograms => Programs;
    }
}
