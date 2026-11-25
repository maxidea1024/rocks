using Rocks.Build;
using Rocks.Build.Rules;

// 월드 컴포넌트와 UI 입력·렌더 타깃을 게임 뷰포트에 연결한다.
public sealed class WorldIntegration : EngineModuleRules
{
    protected override string ModuleName => "WorldIntegration";
    protected override string[] PublicDependencies => ["World", "UiWidget", "UiRender"];
}
