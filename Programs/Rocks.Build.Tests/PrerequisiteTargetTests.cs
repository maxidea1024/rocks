using Rocks.Build;
using Rocks.Build.Rules;

// 선행 타깃(TargetRules.PrerequisiteTargets, Docs/Design/BuildTargets 3.11절 BD44) 시험이다. 양성: 선행 타깃은 자기 규칙(매크로 조합)
// 그대로 주 타깃 전에 나오고, 선행의 선행이 먼저(깊이 우선 후위)이며 같은 타깃은 한 번이다. 조건 칸(플랫폼·구성)으로 거르고, 출시
// 타깃(Game)의 Release는 건너뛰며 Program 주 타깃의 Release는 빌드한다. 음성: 없는 이름·빈 이름·자기 자신·중복·순환·Program이 아닌 선행
// 타깃·빈 조건·조건 값 중복·출시 타깃 Release 명시·선행 타깃을 가진 동반 타깃은 거부한다.
internal static class PrerequisiteTargetTests
{
    public static void Run()
    {
        // Sandbox와 같은 모양: Game 주 타깃이 WithEditorData Program 워커를 Debug·Development에서 먼저 빌드한다.
        FixtureTarget worker = new("Worker", ["WorkerProgram"]) { EditorData = true };
        FixtureTarget game = new("Game", ["GameProgram"])
        {
            Declared = TargetType.Game,
            Prerequisites = [new("Worker") { Configurations = [NativeConfiguration.Debug, NativeConfiguration.Development] }]
        };
        TargetRules[] targets = [game, worker];
        RuleLoader.ValidatePrerequisites(targets);
        IReadOnlyList<TargetRules> built = RuleLoader.PrerequisitesOf(game, targets, NativePlatform.Windows, NativeConfiguration.Debug);
        Check(built.Count == 1 && ReferenceEquals(built[0], worker), "the prerequisite target is built with its own rules");
        Check(built[0].Kind.Defines.Contains("ROCKS_WITH_EDITOR_DATA=1") && game.Kind.Defines.Contains("ROCKS_WITH_EDITOR_DATA=0") &&
            !built[0].Kind.Defines.SequenceEqual(game.Kind.Defines), "the prerequisite keeps its own target macros, not the Game combination");
        Check(built[0].Type == TargetType.Program && built[0].Modules.SequenceEqual(["WorkerProgram"]), "the prerequisite keeps its own kind and modules");
        Check(RuleLoader.PrerequisitesOf(game, targets, NativePlatform.Windows, NativeConfiguration.Development).Count == 1,
            "Development builds the prerequisite");
        // 조건 칸에 없는 구성(Asan·Coverage)과 출시 타깃 Release(조건에 적을 수도 없다)는 건너뛴다.
        foreach (NativeConfiguration configuration in new[] { NativeConfiguration.Asan, NativeConfiguration.Coverage, NativeConfiguration.Release })
        {
            Check(RuleLoader.PrerequisitesOf(game, targets, NativePlatform.Windows, configuration).Count == 0,
                $"{configuration} skips a prerequisite whose condition does not list it");
        }

        // 조건 없는 선행 타깃도 출시 타깃의 Release에서는 건너뛴다(Developer 모듈을 빼는 구성). Program 주 타깃의 Release는 빌드한다.
        FixtureTarget anyGame = new("AnyGame", ["GameProgram"]) { Declared = TargetType.Game, Prerequisites = [new("Worker")] };
        FixtureTarget server = new("AnyServer", ["GameProgram"]) { Declared = TargetType.Server, Prerequisites = [new("Worker")] };
        FixtureTarget tool = new("Tool", ["ToolProgram"]) { Prerequisites = [new("Worker")] };
        TargetRules[] unconditioned = [anyGame, server, tool, worker];
        RuleLoader.ValidatePrerequisites(unconditioned);
        Check(RuleLoader.PrerequisitesOf(anyGame, unconditioned, NativePlatform.Windows, NativeConfiguration.Release).Count == 0,
            "a Release Game target builds no prerequisite targets");
        Check(RuleLoader.PrerequisitesOf(server, unconditioned, NativePlatform.Windows, NativeConfiguration.Release).Count == 0,
            "a Release Server target builds no prerequisite targets");
        Check(RuleLoader.PrerequisitesOf(anyGame, unconditioned, NativePlatform.Windows, NativeConfiguration.Asan).Count == 1,
            "an unconditioned prerequisite is built in every configuration that keeps Developer modules");
        Check(RuleLoader.PrerequisitesOf(tool, unconditioned, NativePlatform.Windows, NativeConfiguration.Release).Count == 1,
            "a Release Program target builds its prerequisite targets");
        Check(!RuleLoader.BuildsPrerequisites(new TargetKind(TargetType.Client), NativeConfiguration.Release) &&
            RuleLoader.BuildsPrerequisites(new TargetKind(TargetType.Editor), NativeConfiguration.Release),
            "Release Client skips and Editor builds (the Developer module rule)");

        // 플랫폼 조건.
        FixtureTarget desktop = new("Desktop", ["GameProgram"])
        {
            Declared = TargetType.Game,
            Prerequisites = [new("Worker") { Platforms = [NativePlatform.Windows, NativePlatform.MacOS] }]
        };
        TargetRules[] platforms = [desktop, worker];
        RuleLoader.ValidatePrerequisites(platforms);
        Check(RuleLoader.PrerequisitesOf(desktop, platforms, NativePlatform.MacOS, NativeConfiguration.Debug).Count == 1 &&
            RuleLoader.PrerequisitesOf(desktop, platforms, NativePlatform.Android, NativeConfiguration.Debug).Count == 0,
            "the platform condition selects the platforms that build the prerequisite");

        // 선행의 선행이 먼저(깊이 우선 후위), 겹치는 타깃은 한 번: A → [B, C], B → [D], C → [D]이면 D, B, C다.
        FixtureTarget d = new("D", ["DProgram"]);
        FixtureTarget b = new("B", ["BProgram"]) { Prerequisites = [new("D")] };
        FixtureTarget c = new("C", ["CProgram"]) { Prerequisites = [new("D")] };
        FixtureTarget a = new("A", ["AProgram"]) { Declared = TargetType.Game, Prerequisites = [new("B"), new("C")] };
        TargetRules[] nested = [a, b, c, d];
        RuleLoader.ValidatePrerequisites(nested);
        Check(RuleLoader.PrerequisitesOf(a, nested, NativePlatform.Windows, NativeConfiguration.Debug).Select(target => target.Name)
            .SequenceEqual(["D", "B", "C"]), "nested prerequisites come first and a shared one is built once");
        Check(RuleLoader.PrerequisitesOf(d, nested, NativePlatform.Windows, NativeConfiguration.Debug).Count == 0,
            "a target without prerequisites builds none");

        // 동반 타깃의 합친 타깃은 선행 타깃을 그대로 갖는다.
        FixtureTarget host = new("Host", ["HostProgram"]) { Companions = ["Plain"], Prerequisites = [new("Worker")] };
        FixtureTarget plain = new("Plain", ["PlainProgram"]);
        TargetRules merged = RuleLoader.WithCompanions(host, [host, plain, worker]);
        Check(merged.PrerequisiteTargets.Count == 1 && merged.PrerequisiteTargets[0].Name == "Worker",
            "the merged companion target keeps the prerequisite targets");

        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Missing")] }], "unknown prerequisite target 'Missing'", "unknown name");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("worker")] }, worker], "unknown prerequisite target 'worker'",
            "name in a different case");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new(" ")] }], "without a name", "blank name");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("G")] }], "twice or itself", "itself");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Worker"), new("Worker")] }, worker], "twice or itself", "duplicate");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Other")] }, new FixtureTarget("Other", ["O"]) { Declared = TargetType.Game }],
            "prerequisite targets are Program targets", "Game prerequisite");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Worker") { Configurations = [] }] }, worker], "Configurations condition",
            "empty configuration condition");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Worker") { Platforms = [NativePlatform.Windows, NativePlatform.Windows] }] },
            worker], "Platforms condition", "duplicated platform condition");
        Rejects([new FixtureTarget("G", ["G"]) { Prerequisites = [new("Worker") { Platforms = [(NativePlatform)99] }] }, worker],
            "Platforms condition", "unknown platform value");
        Rejects([new FixtureTarget("G", ["G"])
            {
                Declared = TargetType.Game,
                Prerequisites = [new("Worker") { Configurations = [NativeConfiguration.Development, NativeConfiguration.Release] }]
            }, worker], "leaves out Developer modules", "Release listed for a Game target");
        Rejects([new FixtureTarget("P", ["P"]) { Prerequisites = [new("Q")] }, new FixtureTarget("Q", ["Q"]) { Prerequisites = [new("P")] }],
            "cycle: P -> Q -> P", "two-target cycle");
        Rejects([new FixtureTarget("P", ["P"]) { Prerequisites = [new("Q")] }, new FixtureTarget("Q", ["Q"]) { Prerequisites = [new("R")] },
            new FixtureTarget("R", ["R"]) { Prerequisites = [new("Q")] }], "cycle: Q -> R -> Q", "cycle below the first target");

        // 동반 타깃은 모듈만 합쳐지므로 선행 타깃을 가질 수 없다(조용히 빠지지 않게).
        FixtureTarget companionWithPrerequisites = new("Companion", ["CompanionProgram"]) { Prerequisites = [new("Worker")] };
        FixtureTarget owner = new("Owner", ["OwnerProgram"]) { Companions = ["Companion"] };
        Expect(() => RuleLoader.WithCompanions(owner, [owner, companionWithPrerequisites, worker]), "has prerequisite targets",
            "companion with prerequisite targets");
    }

    private static void Rejects(TargetRules[] targets, string message, string what) =>
        Expect(() => RuleLoader.ValidatePrerequisites(targets), message, what);

    private static void Expect(Action action, string message, string what)
    {
        try
        {
            action();
        }
        catch (ArgumentException error) when (error.Message.Contains(message, StringComparison.Ordinal))
        {
            return;
        }

        throw new InvalidOperationException($"Prerequisite target check accepted a {what} (expected \"{message}\").");
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
        public IReadOnlyList<PrerequisiteTarget> Prerequisites { get; init; } = [];

        public IReadOnlyList<string> Companions { get; init; } = [];

        public TargetType Declared { get; init; } = TargetType.Program;

        public bool EditorData { get; init; }

        public override string Name => name;

        public override IReadOnlyList<string> Modules => modules;

        public override TargetType Type => Declared;

        public override bool WithEditorData => EditorData;

        public override IReadOnlyList<string> CompanionTargets => Companions;

        public override IReadOnlyList<PrerequisiteTarget> PrerequisiteTargets => Prerequisites;
    }
}
