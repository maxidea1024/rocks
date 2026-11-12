using Rocks.Build;
using Rocks.Build.Rules;

// Direct3D 12 그래픽 backend(Windows)다. 다른 플랫폼에서는 장치 생성이 Unsupported다.
public sealed class D3D12Graphics : EngineModuleRules
{
    protected override string ModuleName => "D3D12Graphics";

    protected override string[] RequiredModules => ["Graphics"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = ModuleName,
            Sources = context.Sources(windows ? $"{backend}/Win64" : $"{backend}/Unsupported"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            // 셰이더는 실행 시 D3DCompile(d3dcompiler_47, 시스템 DLL)로 컴파일한다. 오프라인 DXC 쿠킹은 후속이다.
            SystemLibraries = windows ? ["d3d12", "dxgi", "d3dcompiler", "user32"] : []
        };
    }
}
