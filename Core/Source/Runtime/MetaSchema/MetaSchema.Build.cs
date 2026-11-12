using Rocks.Build.Rules;

// TypeId·FieldId 등 메타 schema 헤더 모듈이다.
public sealed class MetaSchema : CoreModuleRules
{
    protected override string ModuleName => "MetaSchema";
    protected override string[] RequiredModules => ["Core"];
    protected override bool HasSources => false;
}
