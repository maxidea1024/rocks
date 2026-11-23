using Rocks.Build;
using Rocks.Build.Rules;

// Asset index scan benchmark (Docs/Design/AssetSystem section 7.1 item 16, A2d2): serial against parallel Rebuild on a
// synthetic project of fixture packages. Not part of the product runtime.
// AssetTestFixtures (Developer): the TestAsset type the synthetic packages hold.
public sealed class AssetIndexBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "AssetIndexBench",
        ModuleDirectory = "Programs/AssetIndexBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/AssetIndexBench/Private"),
        PublicDependencies = ["Launch", "Core", "Task", "AssetRuntime", "AssetDatabase", "AssetTestFixtures"]
    };
}
