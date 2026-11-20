using Rocks.Build;
using Rocks.Build.Rules;

// Compression throughput benchmark executable (Docs/Design/Compression.ko.md section 8). It does not go into the product runtime.
// Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=CompressionBench /p:Configuration=Release`.
public sealed class CompressionBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "CompressionBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/CompressionBench/Private"),
        PublicDependencies = ["Launch", "Compression", "AsyncCompression"]
    };
}
