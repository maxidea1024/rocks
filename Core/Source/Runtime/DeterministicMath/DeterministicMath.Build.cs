using Rocks.Build.Rules;

// Math functions that produce the same bits regardless of host, CPU or CRT (for cooking and Asset Nugget results). Uses only IEEE basic operations
// and bit operations and never calls CRT math functions. Design: Docs/Design/CoreDataFoundations.ko.md section 2.
public sealed class DeterministicMath : CoreModuleRules
{
    protected override string ModuleName => "DeterministicMath";

    protected override string[] RequiredModules => ["PlatformBase"];
}
