using Rocks.Build.Rules;

// Test module loaded at run time (Docs/Design/ModuleLoader.ko.md section 3.8, ML3). The test executables list it in DynamicModules: the build
// links it into the monolithic executable and adds its module table row, but Load of the executable does not start it and the tests do not see its
// headers. Tests start it with ModuleLoader::Load and use it through the DynamicProbe interface (DynamicModuleTestApi). It is not part of the
// game runtime.
public sealed class DynamicModuleTestFixture : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "DynamicModuleTestFixture";

    protected override string[] PublicDependencies => ["Core", "DynamicModuleTestApi"];
}
