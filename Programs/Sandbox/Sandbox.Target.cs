using Rocks.Build.Rules;

// Engine sample executable target. Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Sandbox`.
public sealed class SandboxTarget : TargetRules
{
    public override string Name => "Sandbox";

    public override IReadOnlyList<string> Modules => ["Sandbox"];

    public override bool CheckPublicHeaders => true;
}
