using Rocks.Build.Rules;

// 설정 계층 시험 fixture 모듈이다(Docs/Design/ConfigSystem.ko.md — 설정 CF1·CF2). Foundation·Engine 시험 실행 파일이 의존하며, 설정 struct
// 리플렉션 헤더와 identity 명세(ConfigTestFixtures.rocksids.json)를 이 모듈이 소유한다. 헤더만 있다. ReflectionTestFixtures와 따로 두어
// 설정 시험 타입이 다른 리플렉션 fixture의 명세·등록과 섞이지 않게 한다.
public sealed class ConfigTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ConfigTestFixtures";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["ConfigTestFixtures/SettingsFixture.h"];
}
