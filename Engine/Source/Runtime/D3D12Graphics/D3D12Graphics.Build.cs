using Rocks.Build;
using Rocks.Build.Rules;

// Direct3D 12 graphics backend (Windows). On other platforms device creation is Unsupported.
public sealed class D3D12Graphics : EngineModuleRules
{
    protected override string ModuleName => "D3D12Graphics";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["DxgiGraphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            ModuleDirectory = ModuleDirectory,
            Unity = UnityFor(context),
            UnityExclude = UnityExclude(context),
            // The entry point (Private/<Module>Module.cpp) is built on every platform.
            Sources = [ModuleEntrySource(context), .. context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported")],
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PublicDependencies = PublicDependencies,
            PrivateDependencies = PrivateDependencies,
            // Shaders are compiled at run time with D3DCompile (d3dcompiler_47, system DLL). Offline DXC cooking comes later.
            SystemLibraries = windows ? ["d3d12", "dxgi", "d3dcompiler", "user32"] : []
        };
    }
}
