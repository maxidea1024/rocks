using Rocks.Build;
using Rocks.Build.Rules;

// Device-free render description values (Docs/Design/Renderer section 3.2). For now only the numbering tables owned by the
// Renderer spec (section 14.2, decision RN5): MaterialDomain, BlendMode and ShadingModel. Material stage MS3b created the module early
// because the material asset fields need these enums (Docs/Design/MaterialSystem section 4.6, request MRD15); Renderer RS0 adds the
// scene description value types. Reflected enums need ObjectRuntime for the generated code. Generated header
// <RenderData/MaterialEnums.generated.h>, module registration rocks::generated::RenderData::RegisterModuleTypes (called by
// RegisterRenderDataTypes), identity spec RenderData.rocksids.json.
public sealed class RenderData : EngineModuleRules
{
    protected override string ModuleName => "RenderData";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime"];
}
