using Rocks.Build;
using Rocks.Build.Rules;

// UI 그리기 데이터를 Graphics 명령으로 전달한다.
public sealed class UiRender : EngineModuleRules
{
    protected override string ModuleName => "UiRender";
    protected override string[] PublicDependencies => ["UiCore", "Graphics", "TextLayout"];
}
