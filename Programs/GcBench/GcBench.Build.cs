using Rocks.Build;
using Rocks.Build.Rules;

// ObjectRegistry GC measurement executable (Docs/Design/GarbageCollectionScale.ko.md GC-S0). Not part of the product runtime.
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development`.
// The common entry layer (Launch) installs crash reporting and turns off OS error dialogs so automated runs never stop at a dialog.
public sealed class GcBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "GcBench",
        ModuleDirectory = "Programs/GcBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/GcBench/Private"),
        PublicDependencies = ["Launch", "ObjectRuntime", "GcSoak"]
    };
}
