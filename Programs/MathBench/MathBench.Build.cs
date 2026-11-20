using Rocks.Build;
using Rocks.Build.Rules;

// Core math microbenchmark executable (Docs/Implementation/Math.ko.md section 5: float vs. double Transform
// cost). It is for measurement, not testing, and is not part of the product runtime.
// Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=MathBench /p:Configuration=Release`.
public sealed class MathBenchProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "MathBench",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/MathBench/Private"),
        Dependencies = ["Launch", "Core"]
    };
}
