using Rocks.Build.Rules;

// 장치 독립 입력 이벤트와 프레임 입력 상태(키보드·마우스·게임패드). OS 입력 backend는 PlatformWindow가
// 이 모듈의 이벤트로 변환해 넣는다. 헤드리스 테스트는 이벤트를 직접 주입한다.
public sealed class Input : EngineModuleRules
{
    protected override string ModuleName => "Input";

    protected override string[] RequiredModules => ["Core"];
}
