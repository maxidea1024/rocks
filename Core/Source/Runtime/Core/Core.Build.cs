using Rocks.Build;
using Rocks.Build.Rules;

// Containers, allocators, strings, math, diagnostics and time (UE Core counterpart). Must not depend on windows or input.
public sealed class Core : CoreModuleRules
{
    protected override string ModuleName => "Core";

    protected override string[] RequiredModules => ["PlatformBase", "DeterministicMath"];

    // Provides Core/Build/BuildInfo.h (rocks::build::ToolchainFingerprint and ToolBuildId). For every executable that reaches this module the build
    // generates and adds a definition TU (Docs/Design/ReflectionFoundations.ko.md section 8.1).
    public override NativeModuleInput Configure(BuildContext context) => base.Configure(context) with { ProvidesBuildInfo = true };
}
