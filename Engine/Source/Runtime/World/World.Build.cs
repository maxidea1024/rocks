using Rocks.Build.Rules;

// World·Actor·Component·SceneComponent·TickScheduler·WorldSubsystem. Engine·Graphics·창 모듈을 참조하지
// 않는다(엔진 실행과 렌더 연결은 상위 모듈이 맡는다). Level·Actor·Component·SceneComponent는 WorldOnly
// 리플렉션 클래스다(Docs/Design/ReflectionFoundations.ko.md 9.1절) — 생성 헤더는 <World/<Type>.generated.h>,
// 집계 등록은 <World/World.module.generated.h>의 rocks::generated::World::RegisterModuleTypes(RegisterWorldTypes가
// 부른다)이고, identity 명세는 World.rocksids.json이다.
public sealed class World : EngineModuleRules
{
    protected override string ModuleName => "World";

    // CoreTypeBindings: SceneComponent의 Transform 필드가 그 모듈의 Binds 미러를 import한다.
    protected override string[] RequiredModules => ["ObjectRuntime", "CoreTypeBindings"];

    protected override IReadOnlyList<string> Metadata =>
        ["World/Level.h", "World/Actor.h", "World/Component.h", "World/SceneComponent.h"];
}
