using Rocks.Build.Rules;

// Spatial index benchmark target. Measure with Release:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=SpatialIndexBench -p:Configuration=Release
public sealed class SpatialIndexBenchTarget : TargetRules
{
    public override string Name => "SpatialIndexBench";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["SpatialIndexBench"];
}
