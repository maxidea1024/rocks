using Rocks.Build.Rules;

// 엔진 런타임 모듈 전체를 빌드하는 타깃이다(UE의 Engine/Source/*.Target.cs 대응). Installed SDK 내보내기의
// 기본 타깃이며, 게임 프로젝트는 이 타깃이 만든 모듈에 의존한다.
public sealed class EngineTarget : TargetRules
{
    public override string Name => "Engine";

    public override IReadOnlyList<string> Modules => ["ObjectSerialization", "Localization"];

    public override bool CheckPublicHeaders => true;
}
