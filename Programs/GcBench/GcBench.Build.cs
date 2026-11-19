using Rocks.Build;
using Rocks.Build.Rules;

// ObjectRegistry GC measurement executable (Docs/Design/GarbageCollectionScale.ko.md GC-S0). Not part of the product runtime.
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development`.
// Links the test-support host failure handler (HostFailure) so automated runs never stop at a dialog.
public sealed class GcBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        var sources = context.Sources("Programs/GcBench/Private").ToList();
        sources.Add(context.PathInRoot(context.Platform == NativePlatform.Windows
            ? $"{support}/Private/Win64/HostFailure.cpp"
            : $"{support}/Private/Posix/HostFailure.cpp"));
        return new()
        {
            Name = "GcBench",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            PrivateIncludeDirectories = [context.PathInRoot($"{support}/Public")],
            Dependencies = ["ObjectRuntime", "GcSoak"]
        };
    }
}
