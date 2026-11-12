using Rocks.Build.Rules;

public sealed class SampleGameTarget : TargetRules
{
    public override string Name => "Game";

    public override IReadOnlyList<string> Modules => ["SampleGame"];
}
