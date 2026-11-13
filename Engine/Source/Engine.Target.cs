using Rocks.Build.Rules;

// 엔진 런타임 모듈 전체를 빌드하는 타깃이다(UE의 Engine/Source/*.Target.cs 대응). Installed SDK 내보내기의
// 기본 타깃이며, 게임 프로젝트는 이 타깃이 만든 모듈에 의존한다.
public sealed class EngineTarget : TargetRules
{
    public override string Name => "Engine";

    // CoreTypeBindings: Core 값 타입 리플렉션 결합(Docs/Design/ReflectionFoundations.ko.md 4.5절) — 게임 모듈이 Core 값
    // 타입 필드를 리플렉션하려면 SDK에 있어야 한다.
    public override IReadOnlyList<string> Modules => ["ObjectSerialization", "Localization", "CoreTypeBindings"];

    public override bool CheckPublicHeaders => true;
}
