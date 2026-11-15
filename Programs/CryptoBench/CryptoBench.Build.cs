using Rocks.Build;
using Rocks.Build.Rules;

// 암호 처리량 측정 실행 파일이다(Docs/Design/Crypto.ko.md 6.4절 성능 gate). 제품 런타임에 들어가지 않는다.
// `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CryptoBench /p:Configuration=Release`로 빌드한다.
public sealed class CryptoBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "CryptoBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/CryptoBench/Private"),
        Dependencies = ["Crypto"]
    };
}
