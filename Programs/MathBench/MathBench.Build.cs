using Rocks.Build;
using Rocks.Build.Rules;

// Core 수학 마이크로벤치마크 실행 파일이다(Docs/Implementation/Math.ko.md 5절 — float·double Transform
// 비용). 시험이 아니라 측정용이며 제품 런타임에 들어가지 않는다.
// `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=MathBench /p:Configuration=Release`로 빌드한다.
public sealed class MathBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "MathBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/MathBench/Private"),
        Dependencies = ["Core"]
    };
}
