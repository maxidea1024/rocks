using Rocks.Build;
using Rocks.Build.Rules;

// 게임 UI 객체와 비객체 Element 트리 사이의 수명·속성 동기화를 맡는다.
public sealed class UiWidget : EngineModuleRules
{
    protected override string ModuleName => "UiWidget";
    protected override string[] PrivateDependencies => ["ObjectSerialization", "PlatformBase"];
    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "Curves", "Localization", "UiFramework", "AssetRuntime", "Material", "World"];
}
