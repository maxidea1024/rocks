using Rocks.Build.Rules;

// ObjectRegistry GC measurement target. Measure with Development (-O2) or higher:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development
public sealed class GcBenchTarget : TargetRules
{
    public override string Name => "GcBench";

    public override IReadOnlyList<string> Modules => ["GcBench"];
}
