using Rocks.Build.Rules;

// ObjectRegistry GC 측정 타깃이다. 수치는 Development(-O2) 이상으로 잰다:
//   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=GcBench -p:Configuration=Development
public sealed class GcBenchTarget : TargetRules
{
    public override string Name => "GcBench";

    public override IReadOnlyList<string> Modules => ["GcBench"];
}
