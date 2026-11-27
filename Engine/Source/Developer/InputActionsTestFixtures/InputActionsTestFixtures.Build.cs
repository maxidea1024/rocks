using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Input actions reflection test fixture module (Docs/Design/InputActions section 5.1, IA7). Owns the header of a reflected class whose
// ROCKS_FUNCTION handlers the function binding tests bind to (one matching handler and mismatching ones), and the identity spec
// (InputActionsTestFixtures.rocksids.json). Only the EngineTests executable depends on it. Header only.
public sealed class InputActionsTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "InputActionsTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader section 3.11). Tests register its types into their own registries;
    // the engine group of a test executable must not register them into every engine.
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    protected override string[] PublicDependencies => ["InputActions"];

    // Config: the generated module registration also registers the reflected types of Input's private dependency Config (as InputActions).
    protected override string[] PrivateDependencies => ["Config"];

    protected override bool HasSources => false;
}
