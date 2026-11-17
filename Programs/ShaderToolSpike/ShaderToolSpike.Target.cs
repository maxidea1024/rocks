using Rocks.Build.Rules;

// Shader tool spike target (SH0). Run on a machine where the tools were bootstrapped:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=ShaderToolSpike /p:Configuration=Debug
//   ShaderToolSpike --out <directory> [--sm 6_0]
public sealed class ShaderToolSpikeTarget : TargetRules
{
    public override string Name => "ShaderToolSpike";

    // Development tool (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ShaderToolSpike"];
}
