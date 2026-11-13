using Rocks.Build.Rules;

// 리플렉션 시험 fixture 모듈이다(Docs/Design/ReflectionFoundations.ko.md 1.9절). Foundation·Engine 시험 실행 파일이
// 의존하며, 리플렉션 헤더와 identity 명세(ReflectionTestFixtures.rocksids.json)를 이 모듈이 소유한다. 헤더만 있다.
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    // CoreTypeBindings: Core 값 타입 필드 fixture(CoreValueFixture.h)가 그 모듈의 Binds 미러를 import한다.
    protected override string[] RequiredModules => ["ObjectRuntime", "CoreTypeBindings"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata =>
        ["ReflectionTestFixtures/ReflectionFixture.h", "ReflectionTestFixtures/AutoIdentityFixture.h", "ReflectionTestFixtures/StructFixture.h",
            "ReflectionTestFixtures/CoreValueFixture.h", "ReflectionTestFixtures/ArrayFixture.h"];
}
