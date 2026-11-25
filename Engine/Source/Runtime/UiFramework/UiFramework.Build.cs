using Rocks.Build;
using Rocks.Build.Rules;

// Game lane의 UI 입력 라우팅이다. 플랫폼과 렌더링은 별도로 연결한다.
public sealed class UiFramework : EngineModuleRules
{
    protected override string ModuleName => "UiFramework";
    protected override string[] PrivateDependencies => ["PlatformBase"];
    protected override string[] PublicDependencies => ["UiCore", "TextLayout"];
}
