using Rocks.Build.Rules;

public sealed class CommandletTarget : TargetRules
{
    public override string Name => "Commandlet";

    public override IReadOnlyList<string> Modules => ["RocksCommandlet"];

    public override bool CheckPublicHeaders => true;
}
