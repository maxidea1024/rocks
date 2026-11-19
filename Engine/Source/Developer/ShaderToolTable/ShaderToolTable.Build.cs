using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// Shader tool table module (Docs/Design/ShaderSystem.ko.md sections 3 and 9). A generated translation unit built from the Engine root <c>kind: binary</c> manifests
/// (<see cref="ShaderToolTableSource"/>) holds tool names, versions, store directory names, file SHA-256 and roles for the build target host. It holds
/// metadata only and never opens files. The main process (Developer/ShaderCompiler) uses this table for job keys and workers use it to open tools. Only
/// ShaderCompiler, workers, ShaderFormat modules and the worker stand-in module ShaderToolLoader may reference this module (policy check shader-tool-table).
/// </summary>
public sealed class ShaderToolTable : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderToolTable";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core"];

    public override NativeModuleInput Configure(BuildContext context) => base.Configure(context) with
    {
        GeneratedSources = [new(ShaderToolTableSource.FileName, ShaderToolTableSource.Generate(context.RootDirectory, HostKey(context)))]
    };

    // Binary host key of the build target (ThirdPartyManifest.HostKeys). Tools run only on development hosts (Windows and macOS), so Android is null.
    private static string? HostKey(BuildContext context)
    {
        string? platform = context.Platform switch
        {
            NativePlatform.Windows => "windows",
            NativePlatform.MacOS => "macos",
            _ => null
        };
        string architecture = context.Architecture == NativeArchitecture.Arm64 ? "arm64" : "x64";
        return platform is null ? null : $"{platform}-{architecture}";
    }
}
