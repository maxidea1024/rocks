using Rocks.Build;
using Rocks.Build.Rules;

// Progress reporting cost benchmark (Docs/Design/ProgressReporting section 15.5, requirement 9). Not part of the product runtime.
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=ProgressBench -p:Configuration=Release`.
public sealed class ProgressBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "ProgressBench",
        ModuleDirectory = "Programs/ProgressBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/ProgressBench/Private"),
        PublicDependencies = ["Launch", "Core", "Localization", "Progress"]
    };
}
