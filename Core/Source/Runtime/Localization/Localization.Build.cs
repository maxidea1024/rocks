using Rocks.Build.Rules;

// 지역화 Text·catalog·culture snapshot.
public sealed class Localization : CoreModuleRules
{
    protected override string ModuleName => "Localization";
    protected override string[] RequiredModules => ["Core"];
}
