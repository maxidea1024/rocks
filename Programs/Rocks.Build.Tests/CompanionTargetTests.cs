using Rocks.Build;
using Rocks.Build.Rules;

// 동반 타깃(TargetRules.CompanionTargets, Docs/Design/ShaderSystem 11.1절 SC14) 시험이다: 동반 프로그램 타깃의 모듈이 같은 빌드에
// 더해지고 나머지 속성은 원래 타깃 그대로이며, 없는 타깃·중복·자기 자신·Program이 아님·Modular·플러그인·동반의 동반·Modular 타깃은
// 거부한다.
internal static class CompanionTargetTests
{
    public static void Run()
    {
        FixtureTarget worker = new("Worker", ["WorkerProgram"]);
        FixtureTarget host = new("Host", ["HostTests", "Probe"]) { Companions = ["Worker"], Test = "HostTests", Programs = ["Launch"] };
        TargetRules[] targets = [worker, host];
        TargetRules merged = RuleLoader.WithCompanions(host, targets);
        Check(merged.Modules.SequenceEqual(["HostTests", "Probe", "WorkerProgram"]), "companion modules are appended after the target modules");
        Check(merged.Name == "Host" && merged.TestExecutable == "HostTests" && merged.TestPrograms.SequenceEqual(["Launch"]) &&
            merged.Type == TargetType.Program &&
            merged.LinkType == NativeLinkType.Monolithic, "the merged target keeps the target's own properties");
        Check(ReferenceEquals(RuleLoader.WithCompanions(worker, targets), worker), "a target without companions is returned as is");

        // 동반 타깃의 모듈이 이미 목록에 있으면 한 번만 둔다.
        FixtureTarget overlapping = new("Overlap", ["WorkerProgram", "Main"]) { Companions = ["Worker"] };
        Check(RuleLoader.WithCompanions(overlapping, [worker, overlapping]).Modules.SequenceEqual(["WorkerProgram", "Main"]),
            "a module already in the target is not listed twice");

        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Missing"] }, [worker], "unknown companion");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Worker", "Worker"] }, [worker], "duplicate companion");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["A"] }, [worker], "self companion");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Game"] }, [new FixtureTarget("Game", ["G"]) { Declared = TargetType.Game }],
            "game companion");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Shared"] },
            [new FixtureTarget("Shared", ["S"]) { Link = NativeLinkType.Modular }], "modular companion");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Nested"] }, [new FixtureTarget("Nested", ["N"]) { Companions = ["Worker"] }, worker],
            "companion with companions");
        Rejects(new FixtureTarget("A", ["A"]) { Companions = ["Worker"], Link = NativeLinkType.Modular }, [worker], "modular target");
    }

    private static void Rejects(TargetRules target, TargetRules[] others, string what)
    {
        try
        {
            RuleLoader.WithCompanions(target, [target, .. others]);
        }
        catch (ArgumentException)
        {
            return;
        }

        throw new InvalidOperationException($"Companion target check accepted a {what}.");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class FixtureTarget(string name, IReadOnlyList<string> modules) : TargetRules
    {
        public IReadOnlyList<string> Companions { get; init; } = [];

        public TargetType Declared { get; init; } = TargetType.Program;

        public NativeLinkType Link { get; init; } = NativeLinkType.Monolithic;

        public string? Test { get; init; }

        public IReadOnlyList<string> Programs { get; init; } = [];

        public override string Name => name;

        public override IReadOnlyList<string> Modules => modules;

        public override TargetType Type => Declared;

        public override NativeLinkType LinkType => Link;

        public override string? TestExecutable => Test;

        public override IReadOnlyList<string> TestPrograms => Programs;

        public override IReadOnlyList<string> CompanionTargets => Companions;
    }
}
