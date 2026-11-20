using Rocks.Build;
using Rocks.Build.Rules;

// Direct3D 11 graphics backend (Windows, feature level 11_0 or later). Device creation returns Unsupported on
// other platforms. Design: Docs/Design/EngineRuntime.ko.md section 10.2.
public sealed class D3D11Graphics : EngineModuleRules
{
    protected override string ModuleName => "D3D11Graphics";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics", "DxgiGraphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            // The entry point (Private/<Module>Module.cpp) is built on every platform.
            Sources = [ModuleEntrySource(context), .. context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported")],
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            // Shaders are compiled at run time with D3DCompile (d3dcompiler_47, a system DLL) until cooked
            // D3D11_SM5 code arrives in SH5.
            SystemLibraries = windows ? ["d3d11", "dxgi", "d3dcompiler", "user32"] : []
        };
    }
}
