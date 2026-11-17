using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    public override bool CheckPublicHeaders => true;
}
