using Rocks.Build.Rules;

// Progress reporting cost benchmark target. Measure with a Release build:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=ProgressBench -p:Configuration=Release
public sealed class ProgressBenchTarget : TargetRules
{
    public override string Name => "ProgressBench";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ProgressBench"];
}
