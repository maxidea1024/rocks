using Rocks.Build;
using Rocks.Build.Rules;

// Modular link check target (Docs/Design/BuildTargets.ko.md section 3.8, BS8d2): builds the FoundationTests module closure with Modular
// linking and one executable. Build and run on Windows:
//   dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=ModularCheck /p:Configuration=Debug
public sealed class ModularCheckTarget : TargetRules
{
    public override string Name => "ModularCheck";

    // Development tool executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override NativeLinkType LinkType => NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["ModularCheck"];
}
