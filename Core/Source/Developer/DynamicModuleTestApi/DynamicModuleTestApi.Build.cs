using Rocks.Build.Rules;

// Public interface of the run-time loaded test module DynamicModuleTestFixture (Docs/Design/ModuleLoader.ko.md section 3.8, ML3). The test
// executables link this header-only module and reach the fixture only through ModuleLoader::Find<DynamicProbe>(name); the fixture is in their
// DynamicModules, so its own headers are not on their include path. It is not part of the game runtime.
public sealed class DynamicModuleTestApi : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "DynamicModuleTestApi";

    protected override string[] RequiredModules => ["Core"];

    protected override bool HasSources => false;
}
