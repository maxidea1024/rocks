using Rocks.Build;
using Rocks.Build.Rules;

// Engine sample executable target. Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Sandbox`.
public sealed class SandboxTarget : TargetRules
{
    public override string Name => "Sandbox";

    public override IReadOnlyList<string> Modules => ["Sandbox"];

    public override bool CheckPublicHeaders => true;

    // The development shader provider starts ShaderCompileWorker (Docs/Design/ShaderSystem section 16.1 SSD19). Sandbox is a Game target, so the
    // worker is built before it with its own rules and output (Docs/Design/BuildTargets section 3.11); Sandbox finds it in the sibling output.
    // Release Sandbox has no provider and Android Sandbox does not start worker processes.
    public override IReadOnlyList<PrerequisiteTarget> PrerequisiteTargets =>
    [
        new("ShaderCompileWorker")
        {
            Platforms = [NativePlatform.Windows, NativePlatform.MacOS],
            Configurations = [NativeConfiguration.Debug, NativeConfiguration.Development]
        }
    ];
}
