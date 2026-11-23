using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Engine module test executable target. Foundation tests up to the Object family; Core tests only the lower modules without Object.
// The ReflectionTestFixtures module owns the reflection fixtures and their identity spec (the test executable depends on that module).
public sealed class FoundationTarget : TargetRules
{
    public override string Name => "Foundation";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["FoundationTests", .. LaunchTestPrograms.Names];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "FoundationTests";
}

// Engine layer (World, Engine, assets) test target. Also runs all Foundation tests. Tests that need a GPU do not go into
// this target (Sandbox --self-test).
public sealed class EngineTestsTarget : TargetRules
{
    public override string Name => "EngineTests";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["EngineTests", .. LaunchTestPrograms.Names];

    // Engine plugins whose module tests EngineTests runs (FoundationTests.Build.cs PluginTestModules, Docs/Design/BuildTargets section 3.9 BD40).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "EngineTests";

    // The shader worker channel tests start ShaderCompileWorker next to the test executable (Docs/Design/ShaderSystem section 11.1 SC14).
    public override IReadOnlyList<string> CompanionTargets => ["ShaderCompileWorker"];
}

// SDK target holding only the reflection fixture modules (and their dependencies). Exported when Rocks.Build.IntegrationTests tests SDK descriptor import
// (an external game module derives from and references reflection types of SDK modules without regenerating those modules).
// ObjectSerialization: the game saves and restores array fields of SDK types as a graph (B5c integration test).
// Launch: the game template's entry point (ROCKS_IMPLEMENT_APPLICATION).
public sealed class ReflectionFixturesTarget : TargetRules
{
    public override string Name => "ReflectionFixtures";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ReflectionTestFixtures", "ObjectSerialization", "Launch"];
}

public sealed class CoreTarget : TargetRules
{
    public override string Name => "Core";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["CoreFoundationTests", .. LaunchTestPrograms.Names];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "CoreFoundationTests";
}

// Modular build of the Foundation tests (Docs/Design/BuildTargets section 3.8 BD31, BS8d5): every non-third-party library module is a
// shared library and the test executable starts through the Modular entry (Docs/Design/ModuleLoader.ko.md section 3.14). One executable only
// (a Modular target builds one executable: the Launch test programs are the companion targets in TestPrograms, BS8d6, and the embedded entry
// program stays in the Monolithic targets). Tests stay in the FoundationTests host module and the module internals they call are exported.
public sealed class FoundationModularTarget : TargetRules
{
    public override string Name => "FoundationModular";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["FoundationTests"];

    public override string TestExecutable => "FoundationTests";

    public override IReadOnlyList<string> TestPrograms => LaunchTestPrograms.ModularTargets;
}

// Launch test executables (Programs/LaunchTests). Every test target builds them so the native harness (Native.Launch.*) finds them next to
// the test executable.
internal static class LaunchTestPrograms
{
    public static readonly string[] Names = ["LaunchProbe", "LaunchWindowedProbe", "LaunchEmbeddedHost"];

    // Modular companion targets of the Modular test targets (Docs/Design/BuildTargets section 3.8 BS8d6). The embedded entry program has
    // none: the embedded entry does not support Modular (Docs/Design/ModuleLoader.ko.md section 3.14).
    public static readonly string[] ModularTargets = ["LaunchProbeModular", "LaunchWindowedProbeModular"];
}

// Modular builds of the Launch test programs (Docs/Design/BuildTargets section 3.8 BS8d6): one executable per Modular target, like UE
// Modular programs. The Modular test targets build them through TestPrograms.
public sealed class LaunchProbeModularTarget : TargetRules
{
    public override string Name => "LaunchProbeModular";

    // Development test program (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["LaunchProbe"];
}

public sealed class LaunchWindowedProbeModularTarget : TargetRules
{
    public override string Name => "LaunchWindowedProbeModular";

    // Development test program (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["LaunchWindowedProbe"];
}

// Modular build of the engine tests (Docs/Design/BuildTargets section 3.8 BS8e): the engine layer modules link as shared libraries and the
// test executable starts through the Modular entry. The test module is EngineModularTests (the EngineTests sources). The enabled engine plugins
// link into their plugin binaries folders with their own module manifests (Docs/Design/ModuleLoader.ko.md section 3.15 ML5a).
public sealed class EngineModularTarget : TargetRules
{
    public override string Name => "EngineModular";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["EngineModularTests"];

    // Engine plugins whose module tests EngineModularTests runs (the same as EngineTests).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override string TestExecutable => "EngineModularTests";

    public override IReadOnlyList<string> TestPrograms => LaunchTestPrograms.ModularTargets;
}
