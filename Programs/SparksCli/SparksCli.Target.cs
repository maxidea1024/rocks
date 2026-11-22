using Rocks.Build.Rules;

// The Sparks command line tool target (Docs/Design/SparksTools.ko.md, LC8).
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=SparksCli
public sealed class SparksCliTarget : TargetRules
{
    public override string Name => "SparksCli";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["SparksCli"];
}
