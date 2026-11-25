using Rocks.Build.Rules;

// The Sparks language server target (Docs/Design/SparksTools.ko.md section 10, LC8e).
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=SparksLsp
public sealed class SparksLspTarget : TargetRules
{
    public override string Name => "SparksLsp";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["SparksLsp"];
}
