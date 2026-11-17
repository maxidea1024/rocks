using Rocks.Build.Rules;

// 작업 DAG·worker scheduler(UE TaskGraph 대응). task 수명 사건을 TraceCapture에 기록한다(채널 task,
// Docs/Design/TraceSystem.ko.md 7절). TraceCapture는 Task에 의존하지 않는다.
public sealed class TaskRuntime : CoreModuleRules
{
    protected override string ModuleName => "Task";

    protected override string[] RequiredModules => ["Core", "TraceCapture"];
}
