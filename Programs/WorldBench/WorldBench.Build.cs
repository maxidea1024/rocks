using Rocks.Build;
using Rocks.Build.Rules;

// World scale measurement executable (Docs/Implementation/GcScaleExperiment.ko.md linear search audit). Not part of the
// product runtime. Measures per-operation World costs at several actor counts.
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=WorldBench -p:Configuration=Development`.
public sealed class WorldBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        var sources = context.Sources("Programs/WorldBench/Private").ToList();
        sources.Add(context.PathInRoot(context.Platform == NativePlatform.Windows
            ? $"{support}/Private/Win64/HostFailure.cpp"
            : $"{support}/Private/Posix/HostFailure.cpp"));
        return new()
        {
            Name = "WorldBench",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            PrivateIncludeDirectories = [context.PathInRoot($"{support}/Public")],
            Dependencies = ["World"]
        };
    }
}
