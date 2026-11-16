using Rocks.Build.Rules;

// 장치에 묶인 렌더 기반이다(Docs/Design/RenderBase.ko.md, UE RenderCore 일부 대응): 렌더 lane과 프레임
// slot·프레임 packet 전달, 렌더 전달 가능 타입 등록, 완료 통지, RenderHandle 예약 표. Graphics 장치 계약
// 위에 있고 ObjectRuntime·World·Engine을 모른다(게임 쪽 연결은 실행 파일·WorldIntegration이 한다).
// MetaSchema는 전달 타입의 Object 참조 필드 검사(Development)에만 쓴다.
public sealed class RenderBase : EngineModuleRules
{
    protected override string ModuleName => "RenderBase";

    protected override string[] RequiredModules => ["Graphics", "Task", "MetaSchema"];
}
