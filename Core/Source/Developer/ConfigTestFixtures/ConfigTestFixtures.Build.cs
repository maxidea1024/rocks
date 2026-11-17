using Rocks.Build.Rules;

// Config layer test fixture module (Docs/Design/ConfigSystem.ko.md, config CF1 and CF2). The Foundation and Engine test executables depend on it.
// This module owns the settings struct reflection headers and the identity spec (ConfigTestFixtures.rocksids.json). Header only. It is kept
// separate from ReflectionTestFixtures so config test types do not mix with the specs and registration of other reflection fixtures.
public sealed class ConfigTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ConfigTestFixtures";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["ConfigTestFixtures/SettingsFixture.h"];
}
