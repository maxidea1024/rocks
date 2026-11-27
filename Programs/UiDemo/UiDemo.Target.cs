using Rocks.Build;
using Rocks.Build.Rules;

// Example that connects the UI runtime to a real window. Build with `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=UiDemo`.
public sealed class UiDemoTarget : TargetRules
{
    public override string Name => "UiDemo";

    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["UiDemo"];

    public override bool CheckPublicHeaders => true;

    // The development shader provider starts ShaderCompileWorker (Docs/Design/ShaderSystem section 16.1 SSD19). The worker is built before the
    // demo with its own rules and output (Docs/Design/BuildTargets section 3.11); the demo finds it in the sibling output.
    public override IReadOnlyList<PrerequisiteTarget> PrerequisiteTargets =>
    [
        new("ShaderCompileWorker")
        {
            Platforms = [NativePlatform.Windows, NativePlatform.MacOS],
            Configurations = [NativeConfiguration.Debug, NativeConfiguration.Development]
        }
    ];
}
