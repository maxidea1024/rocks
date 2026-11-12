using Rocks.Build.Rules;

// World·Actor·Component·SceneComponent·TickScheduler·WorldSubsystem. Engine·Graphics·창 모듈을 참조하지
// 않는다(엔진 실행과 렌더 연결은 상위 모듈이 맡는다).
public sealed class World : EngineModuleRules
{
    protected override string ModuleName => "World";
    protected override string[] RequiredModules => ["ObjectRuntime"];
}
