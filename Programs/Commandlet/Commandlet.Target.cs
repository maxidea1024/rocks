using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    // Reads and writes editor data: test fixtures and asset tools keep EditorOnly fields (Docs/Design/BuildTargets section 3.4 BS2c — the
    // ROCKS_WITH_EDITOR_DATA=1 test combination).
    public override bool WithEditorData => true;

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    // Engine plugins linked into the commandlet (Docs/Design/BuildTargets section 3.9): UnrealExchange (the asset-import-unreal command,
    // Docs/Design/UnrealExchange section 10.5).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override bool CheckPublicHeaders => true;

    // shader-compile starts ShaderCompileWorker next to the commandlet (Docs/Design/ShaderSystem section 11.1 SC13 and SC14). Commandlet is a
    // Monolithic Program, so the worker is a companion target, not a TestPrograms entry (that list is for Modular test targets).
    public override IReadOnlyList<string> CompanionTargets => ["ShaderCompileWorker"];
}
