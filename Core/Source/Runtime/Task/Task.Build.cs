using Rocks.Build.Rules;

// 작업 DAG·worker scheduler(UE TaskGraph 대응).
public sealed class TaskRuntime : CoreModuleRules
{
    protected override string ModuleName => "Task";

    protected override string[] RequiredModules => ["Core"];
}
