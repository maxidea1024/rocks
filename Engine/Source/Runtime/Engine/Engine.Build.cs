using Rocks.Build.Rules;

// 엔진 실행 상태 기계·메인 루프·World 실행 문맥·EngineSubsystem. 창·그래픽·게임플레이를 모른다
// (이벤트 펌프·렌더는 EngineLoopHooks, 게임 세션은 EngineClient 확장 지점으로 연결한다).
public sealed class Engine : EngineModuleRules
{
    protected override string ModuleName => "Engine";
    protected override string[] RequiredModules => ["World"];
}
