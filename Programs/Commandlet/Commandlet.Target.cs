using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    // Engine plugins linked into the commandlet (Docs/Design/BuildTargets.ko.md section 3.9): UnrealExchange (asset-import-unreal, its command
    // registration comes with stage UX1b).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override bool CheckPublicHeaders => true;
}
