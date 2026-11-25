using Rocks.Build;
using Rocks.Build.Rules;

// 입력, UI, Graphics를 연결하는 독립 예제다.
public sealed class UiDemoProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "UiDemo",
        ModuleDirectory = "Programs/UiDemo",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/UiDemo/Private"),
        PublicDependencies = ["Launch", "PlatformWindow", "D3D11Graphics", "UiFramework", "UiRender", "UiWidget"],
        PrivateDependencies = ["AssetDatabase", "WorldIntegration"]
    };
}
