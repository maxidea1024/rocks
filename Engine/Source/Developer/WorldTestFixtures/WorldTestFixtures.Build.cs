using Rocks.Build.Rules;

// World 리플렉션 시험 fixture 모듈이다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8b). World 모듈의 WorldOnly
// 리플렉션 클래스를 상속한 타입(SaveActor·SaveComponent)의 헤더와 identity 명세(WorldTestFixtures.rocksids.json)를
// 소유한다. EngineTests 실행 파일만 의존하며 헤더만 있다.
public sealed class WorldTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "WorldTestFixtures";

    protected override string[] RequiredModules => ["World"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["WorldTestFixtures/SaveFixture.h"];
}
