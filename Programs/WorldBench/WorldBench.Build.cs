using Rocks.Build;
using Rocks.Build.Rules;

// World scale measurement executable (Docs/Implementation/GcScaleExperiment.ko.md linear search audit). Not part of the
// product runtime. Measures per-operation World costs at several actor counts.
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=WorldBench -p:Configuration=Development`.
public sealed class WorldBenchProgram : ModuleRules
{
    // The common entry layer (Launch) installs crash reporting and turns off OS error dialogs so automated runs never stop at a dialog.
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "WorldBench",
        ModuleDirectory = "Programs/WorldBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/WorldBench/Private"),
        PublicDependencies = ["Launch", "World"]
    };
}
