using Rocks.Build.Rules;

// Graphics value types such as pixel formats, samplers and mip sizes. Shared by the device contract (Graphics), assets (AssetRuntime)
// and tools. Does not reference devices, windows or World.
public sealed class GraphicsTypes : EngineModuleRules
{
    protected override string ModuleName => "GraphicsTypes";

    protected override string[] RequiredModules => ["Core"];
}
