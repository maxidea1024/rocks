using Rocks.Build.Rules;

// ObjectRegistry GC measurement target. Measure with Development (-O2) or higher:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development
public sealed class GcBenchTarget : TargetRules
{
    public override string Name => "GcBench";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["GcBench"];
}
