using Rocks.Build;
using Rocks.Build.Rules;

// Log producer cost benchmark (Docs/Design/LoggingSystem.ko.md 8.6). Not part of the product runtime.
// Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=LogBench /p:Configuration=Release`.
public sealed class LogBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "LogBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/LogBench/Private"),
        Dependencies = ["Core"]
    };
}
