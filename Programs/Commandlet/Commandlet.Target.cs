using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    // Engine plugins linked into the commandlet (Docs/Design/BuildTargets.ko.md section 3.9): UnrealExchange (the asset-import-unreal command,
    // Docs/Design/UnrealExchange.ko.md section 10.5).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override bool CheckPublicHeaders => true;
}
