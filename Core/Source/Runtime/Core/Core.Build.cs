using Rocks.Build;
using Rocks.Build.Rules;

// Containers, allocators, strings, math, diagnostics and time (UE Core counterpart). Must not depend on windows or input.
public sealed class Core : CoreModuleRules
{
    protected override string ModuleName => "Core";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Shared PCH: modules that reach Core and have no module PCH precompile this header with their own arguments
    // (Docs/Design/UnityBuildPch.ko.md section 8.4). The header lists widely included, rarely changed Base, PlatformBase and Core headers.
    protected override string? SharedPchHeader => "Core/CoreSharedPch.h";

    protected override string[] RequiredModules => ["PlatformBase", "DeterministicMath"];

    // Provides Core/Build/BuildInfo.h (rocks::build::ToolchainFingerprint and ToolBuildId). For every executable that reaches this module the build
    // generates and adds a definition TU (Docs/Design/ReflectionFoundations.ko.md section 8.1).
    // Also provides Core/Module/ModuleEntry.h (ROCKS_IMPLEMENT_MODULE and the module table types). For every executable that reaches this module the
    // build generates the module table TU, and every C++ module that reaches it gets ROCKS_MODULE_NAME (Docs/Design/ModuleLoader.ko.md section 3.2).
    public override NativeModuleInput Configure(BuildContext context) =>
        base.Configure(context) with { ProvidesBuildInfo = true, ProvidesModuleTable = true };
}
