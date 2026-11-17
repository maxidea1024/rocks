using Rocks.Build.Rules;

// Compression throughput benchmark target. Measure with Release:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CompressionBench /p:Configuration=Release
public sealed class CompressionBenchTarget : TargetRules
{
    public override string Name => "CompressionBench";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["CompressionBench"];
}
