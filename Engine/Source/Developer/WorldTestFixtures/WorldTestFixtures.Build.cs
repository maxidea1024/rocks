using Rocks.Build.Rules;

// World reflection test fixture module (Docs/Design/ReflectionFoundations.ko.md section 9.1, B8b). Owns the headers of types that derive from the World module's
// WorldOnly reflection classes (SaveActor and SaveComponent) and the identity spec (WorldTestFixtures.rocksids.json).
// Only the EngineTests executable depends on it. Header only.
public sealed class WorldTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "WorldTestFixtures";

    protected override string[] RequiredModules => ["World"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["WorldTestFixtures/SaveFixture.h"];
}
