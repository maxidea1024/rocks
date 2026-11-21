using Rocks.Build.Rules;

public sealed class FoundationTemplateTarget : TargetRules
{
    public override string Name => "FoundationTemplate";

    public override IReadOnlyList<string> Modules => ["FoundationTemplate"];
}
