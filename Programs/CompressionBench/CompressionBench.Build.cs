using Rocks.Build;
using Rocks.Build.Rules;

// 압축 처리량 측정 실행 파일이다(Docs/Design/Compression.ko.md 8절). 제품 런타임에 들어가지 않는다.
// `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CompressionBench /p:Configuration=Release`로 빌드한다.
public sealed class CompressionBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "CompressionBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/CompressionBench/Private"),
        Dependencies = ["Compression"]
    };
}
