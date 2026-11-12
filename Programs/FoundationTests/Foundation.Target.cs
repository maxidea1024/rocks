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

// 엔진 계층(World·Engine·에셋) 테스트 타깃이다. Foundation 테스트 전체를 함께 실행하므로 Object 계열
// fixture의 메타 입력과 ID 명세도 같이 쓴다. GPU가 필요한 시험은 이 타깃에 넣지 않는다(Sandbox --self-test).
public sealed class EngineTestsTarget : TargetRules
{
    public override string Name => "EngineTests";
    public override IReadOnlyList<string> Modules => ["EngineTests"];
    public override string? IdentityManifest => new FoundationTarget().IdentityManifest;
    // 같은 fixture를 이 타깃의 테스트 실행 파일이 소유한다(메타 입력은 타깃의 모듈에서 도달해야 한다).
    public override IReadOnlyList<MetadataUnit> Metadata =>
        new FoundationTarget().Metadata.Select(unit => unit with { Module = "EngineTests" }).ToArray();
    public override bool CheckPublicHeaders => true;
    public override string TestExecutable => "EngineTests";
}

public sealed class CoreTarget : TargetRules
{
    public override string Name => "Core";
    public override IReadOnlyList<string> Modules => ["CoreFoundationTests"];
    public override bool CheckPublicHeaders => true;
    public override string TestExecutable => "CoreFoundationTests";
}
