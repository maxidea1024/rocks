using Rocks.Build.Rules;

// 로컬 플레이어의 화면 수명과 입력 우선순위를 관리한다.
public sealed class UiScreens : EngineModuleRules
{
    protected override string ModuleName => "UiScreens";
    protected override string[] PublicDependencies => ["UiWidget", "Input"];
}
