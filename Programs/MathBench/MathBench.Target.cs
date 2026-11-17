using Rocks.Build.Rules;

// Core math microbenchmark target. Measure numbers with Release:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=MathBench /p:Configuration=Release
public sealed class MathBenchTarget : TargetRules
{
    public override string Name => "MathBench";

    // Development tool and measurement executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["MathBench"];
}
