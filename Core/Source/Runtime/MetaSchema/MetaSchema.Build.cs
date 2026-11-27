using Rocks.Build.Rules;

// Header module for meta schema types such as TypeId and FieldId, and the lane transfer registration with its reflection
// schema check (Meta/LaneTransfer.h, Docs/Design/RenderBase section 5.2) for modules that hand values to another lane without
// depending on RenderBase.
public sealed class MetaSchema : CoreModuleRules
{
    protected override string ModuleName => "MetaSchema";

    protected override string[] PublicDependencies => ["Core"];

    protected override bool HasSources => false;
}
