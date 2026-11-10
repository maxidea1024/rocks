using Rocks.Build.Rules;

public sealed class EngineTarget : TargetRules
{
    public override string Name => "Engine";
    public override IReadOnlyList<string> Modules => ["ObjectSerialization", "Localization"];
    public override bool CheckPublicHeaders => true;
}

public sealed class FoundationTarget : TargetRules
{
    public override string Name => "Foundation";
    public override IReadOnlyList<string> Modules => ["FoundationTests"];
    public override string? IdentityManifest => "Tests/Reflection/Foundation.rocksids.json";
    public override IReadOnlyList<MetadataUnit> Metadata =>
        [new("FoundationTests", "Tests/Reflection/ReflectionFixture.h", "Tests/Reflection/ReflectionFixture.h", "ReflectionFixture.generated.h"),
         new("FoundationTests", "Tests/Reflection/AutoIdentityFixture.h", "Tests/Reflection/AutoIdentityFixture.h", "AutoIdentityFixture.generated.h")];
    public override bool CheckPublicHeaders => true;
    public override string TestExecutable => "FoundationTests";
}

public sealed class CoreTarget : TargetRules
{
    public override string Name => "Core";
    public override IReadOnlyList<string> Modules => ["CoreFoundationTests"];
    public override bool CheckPublicHeaders => true;
    public override string TestExecutable => "CoreFoundationTests";
}
