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

    // Engine plugins whose module tests EngineTests runs (FoundationTests.Build.cs PluginTestModules, Docs/Design/BuildTargets.ko.md section 3.9 BD40).
    public override IReadOnlyList<PluginReference> Plugins => [new("UnrealExchange", true)];

    public override bool CheckPublicHeaders => true;

    public override string TestExecutable => "EngineTests";

    // The shader worker channel tests start ShaderCompileWorker next to the test executable (Docs/Design/ShaderSystem.ko.md section 11.1 SC14).
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

// Modular build of the Foundation tests (Docs/Design/BuildTargets.ko.md section 3.8 BD31, BS8d5): every non-third-party library module is a
// shared library and the test executable starts through the Modular entry (Docs/Design/ModuleLoader.ko.md section 3.14). One executable only
// (the Launch test programs and the embedded entry program stay in the Monolithic targets). Tests stay in the FoundationTests host module and the
// module internals they call are exported.
public sealed class FoundationModularTarget : TargetRules
{
    public override string Name => "FoundationModular";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["FoundationTests"];

    public override string TestExecutable => "FoundationTests";
}

// Launch test executables (Programs/LaunchTests). Every test target builds them so the native harness (Native.Launch.*) finds them next to
// the test executable.
internal static class LaunchTestPrograms
{
    public static readonly string[] Names = ["LaunchProbe", "LaunchWindowedProbe", "LaunchEmbeddedHost"];
}

// Modular build of the engine tests (Docs/Design/BuildTargets.ko.md section 3.8 BS8e): the engine layer modules link as shared libraries and the
// test executable starts through the Modular entry. The test module is EngineModularTests (the EngineTests sources without the engine plugin tests:
// Modular targets cannot enable plugins until the plugin binary manifest, ModuleLoader ML5).
public sealed class EngineModularTarget : TargetRules
{
    public override string Name => "EngineModular";

    // Development tool and test executable (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override Rocks.Build.NativeLinkType LinkType => Rocks.Build.NativeLinkType.Modular;

    public override IReadOnlyList<string> Modules => ["EngineModularTests"];

    public override string TestExecutable => "EngineModularTests";
}
