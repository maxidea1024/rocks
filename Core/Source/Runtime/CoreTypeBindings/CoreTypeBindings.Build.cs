using Rocks.Build.Rules;

// Core 값 타입(벡터·쿼터니언·회전·색·정수 벡터·Transform)의 리플렉션 결합 모듈이다(Docs/Design/
// ReflectionFoundations.ko.md 4.5절). Core는 MetaSchema·ObjectRuntime에 의존하지 않으므로 Core 헤더에 annotation을
// 달 수 없다 — 이 모듈의 미러 선언(ROCKS_STRUCT(Binds = "…"))이 같은 공개 멤버를 리플렉션하고, 생성 코드는 실제
// Core 타입의 GeneratedType을 낸다. Binds는 이 모듈에서만 쓸 수 있다. Core 값 타입 필드를 리플렉션하는 모듈은 이
// 모듈에 의존한다. 헤더만 있다.
public sealed class CoreTypeBindings : CoreModuleRules
{
    protected override string ModuleName => "CoreTypeBindings";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["CoreTypeBindings/CoreValueTypes.h"];
}
