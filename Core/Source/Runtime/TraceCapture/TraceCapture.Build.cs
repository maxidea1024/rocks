using Rocks.Build.Rules;

// 다중 스레드 trace 수집(UE TraceLog 대응, Docs/Design/TraceSystem.ko.md): 세션·채널·이벤트 형·스레드 블록과 풀·
// 정의 버퍼와 저장소·writer·출력. writer는 Task가 아니라 플랫폼 스레드다(일반 scheduler가 끝난 뒤에도 비워야 한다).
public sealed class TraceCapture : CoreModuleRules
{
    protected override string ModuleName => "TraceCapture";

    protected override string[] RequiredModules => ["Core", "Compression"];
}
