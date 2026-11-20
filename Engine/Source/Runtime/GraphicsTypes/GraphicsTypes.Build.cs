using Rocks.Build;
using Rocks.Build.Rules;

// Graphics value types such as pixel formats, samplers and mip sizes. Shared by the device contract (Graphics), assets (AssetRuntime)
// and tools. Does not reference devices, windows or World.
public sealed class GraphicsTypes : EngineModuleRules
{
    protected override string ModuleName => "GraphicsTypes";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];
}
