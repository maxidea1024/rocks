using Rocks.Build.Rules;

// Header module for meta schema types such as TypeId and FieldId.
public sealed class MetaSchema : CoreModuleRules
{
    protected override string ModuleName => "MetaSchema";

    protected override string[] RequiredModules => ["Core"];

    protected override bool HasSources => false;
}
