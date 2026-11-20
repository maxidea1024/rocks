using Rocks.Build;
using Rocks.Build.Rules;

// Spatial index benchmark executable (Docs/Design/SpatialIndex.ko.md section 9, OT5, SIP8). It does not go into the product runtime or
// any test executable. Private/UnityOctreePort.* is a BSD-2-Clause re-implementation of UnityOctree used only here (registered in
// Engine/Source/ThirdParty/UnityOctree/ThirdParty.json).
// Build with `dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=SpatialIndexBench -p:Configuration=Release`.
public sealed class SpatialIndexBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SpatialIndexBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/SpatialIndexBench/Private"),
        PublicDependencies = ["Launch", "SpatialIndex", "Task"]
    };
}
