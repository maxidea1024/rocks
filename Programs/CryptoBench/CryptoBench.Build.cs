using Rocks.Build;
using Rocks.Build.Rules;

// Crypto throughput benchmark executable (Docs/Design/Crypto.ko.md section 6.4, performance gate). It does not go into the product runtime.
// Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CryptoBench /p:Configuration=Release`.
public sealed class CryptoBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "CryptoBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/CryptoBench/Private"),
        Dependencies = ["Launch", "Crypto"]
    };
}
