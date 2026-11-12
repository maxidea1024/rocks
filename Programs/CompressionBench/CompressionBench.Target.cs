using Rocks.Build.Rules;

// 압축 처리량 측정 타깃이다. 수치는 Release로 측정한다:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CompressionBench /p:Configuration=Release
public sealed class CompressionBenchTarget : TargetRules
{
    public override string Name => "CompressionBench";

    public override IReadOnlyList<string> Modules => ["CompressionBench"];
}
