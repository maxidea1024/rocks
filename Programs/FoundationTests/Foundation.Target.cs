using Rocks.Build.Rules;

// 엔진 모듈 테스트 실행 파일 타깃이다. Foundation은 Object 계열까지, Core는 Object 없는 하위 모듈만 시험한다.
public sealed class FoundationTarget : TargetRules
{
    public override string Name => "Foundation";
    public override IReadOnlyList<string> Modules => ["FoundationTests"];
    public override string? IdentityManifest => "Core/Source/Runtime/ObjectRuntime/Tests/Reflection/Foundation.rocksids.json";
    private const string Fixtures = "Core/Source/Runtime/ObjectRuntime/Tests/Reflection";

    public override IReadOnlyList<MetadataUnit> Metadata =>
        [new("FoundationTests", $"{Fixtures}/ReflectionFixture.h", $"{Fixtures}/ReflectionFixture.h", "ReflectionFixture.generated.h"),
         new("FoundationTests", $"{Fixtures}/AutoIdentityFixture.h", $"{Fixtures}/AutoIdentityFixture.h", "AutoIdentityFixture.generated.h")];
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
