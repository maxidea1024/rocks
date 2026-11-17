using Rocks.Build.Rules;

// Text and JSON notation of reflected values and assignment batches (reflection B9i, Docs/Design/ReflectionExtensions.ko.md section 10).
// Kept outside ObjectRuntime so JSON and command line notation are not pulled into every ObjectRuntime user.
public sealed class ReflectionText : CoreModuleRules
{
    protected override string ModuleName => "ReflectionText";

    protected override string[] RequiredModules => ["ObjectRuntime", "Core"];
}
