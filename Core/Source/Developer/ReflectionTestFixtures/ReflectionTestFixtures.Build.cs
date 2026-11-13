using Rocks.Build.Rules;

// 리플렉션 시험 fixture 모듈이다(Docs/Design/ReflectionFoundations.ko.md 1.9절). Foundation·Engine 시험 실행 파일이
// 의존하며, 리플렉션 헤더와 identity 명세(ReflectionTestFixtures.rocksids.json)를 이 모듈이 소유한다. 헤더만 있다.
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata =>
        ["ReflectionTestFixtures/ReflectionFixture.h", "ReflectionTestFixtures/AutoIdentityFixture.h", "ReflectionTestFixtures/StructFixture.h"];
}
