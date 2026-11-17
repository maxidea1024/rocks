using Rocks.Build.Rules;

// 설정 계층과 콘솔 설정이다(Docs/Design/ConfigSystem.ko.md — 설정 CF1·CF2). 엔진·프로젝트·플랫폼·사용자·세션·콘솔 층의 값을 한 우선순위로
// 합치고 값마다 출처 스택을 둔다. 설정 struct(ROCKS_STRUCT(Settings(...)))는 ReflectionText의 JSON import·대입 묶음으로 읽는다.
// 렌더 lane 연결(RenderBase RB1)은 이 모듈 밖이다 — 여기는 설정 snapshot 자료구조와 세대 번호까지만 둔다.
public sealed class Config : CoreModuleRules
{
    protected override string ModuleName => "Config";

    protected override string[] RequiredModules => ["ReflectionText", "ObjectRuntime", "Core"];
}
