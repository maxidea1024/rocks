using Rocks.Build.Rules;

// Log producer cost benchmark target. Measure with a Release build:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=LogBench /p:Configuration=Release
public sealed class LogBenchTarget : TargetRules
{
    public override string Name => "LogBench";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["LogBench"];
}
