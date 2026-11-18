using Rocks.Build.Rules;

// World scale measurement target. Measure with Development (-O2) or higher:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=WorldBench -p:Configuration=Development
public sealed class WorldBenchTarget : TargetRules
{
    public override string Name => "WorldBench";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["WorldBench"];
}
