using Rocks.Build;
using Rocks.Build.Rules;

// 창과 Graphics에 의존하지 않는 UI 좌표 및 배치 기반이다.
public sealed class UiCore : EngineModuleRules
{
    protected override string ModuleName => "UiCore";
    protected override string[] PublicDependencies => ["Core", "Input"];
}
