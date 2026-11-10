using Rocks.Build.Rules;

public sealed class SampleGameTarget : TargetRules
{
    public override string Name => "Game";
    public override IReadOnlyList<string> Modules => ["SampleGame"];
    public override string? IdentityManifest => "Game.rocksids.json";
    public override IReadOnlyList<MetadataUnit> Metadata =>
        [new("SampleGame", "Source/GameObject.h", "Source/GameObject.h", "GameObject.generated.h")];
}
