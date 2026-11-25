using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Reflection test fixture module (Docs/Design/ReflectionFoundations.ko.md section 1.9). The Foundation and Engine test executables
// depend on it, and this module owns the reflection headers and the identity spec (ReflectionTestFixtures.rocksids.json).
public sealed class ReflectionTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ReflectionTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader section 3.11). Tests register its types into their own registries;
    // the engine group of a test executable must not register them into every engine.
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    // CoreTypeBindings: the Core value type field fixture (CoreValueFixture.h) imports that module's Binds mirrors.
    // Localization: rocks::Text fields (TextFieldFixture.h).
    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "Localization"];
}
