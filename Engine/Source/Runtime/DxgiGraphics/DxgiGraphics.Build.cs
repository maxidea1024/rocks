using Rocks.Build;
using Rocks.Build.Rules;

// DXGI adapter helpers shared by the Direct3D 11 and Direct3D 12 backends (HDR output check, guaranteed resource size). The adapter
// queries are Windows only; other platforms build only the platform-neutral part.
public sealed class DxgiGraphics : EngineModuleRules
{
    protected override string ModuleName => "DxgiGraphics";

    protected override string[] RequiredModules => ["Core"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string sources = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            Sources = windows ? context.Sources($"{sources}/Common", $"{sources}/Win64") : context.Sources($"{sources}/Common"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            SystemLibraries = windows ? ["dxgi"] : []
        };
    }
}
