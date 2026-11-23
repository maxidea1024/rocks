using Rocks.Build.Rules;

// Asset index scan benchmark target (Docs/Design/AssetSystem section 7.1 item 16, A2d2). Measure with a Release build:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=AssetIndexBench /p:Configuration=Release
public sealed class AssetIndexBenchTarget : TargetRules
{
    public override string Name => "AssetIndexBench";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["AssetIndexBench"];
}
