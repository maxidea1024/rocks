using Rocks.Build;
using Rocks.Build.Rules;

// Material graph schema (Docs/Design/MaterialSystem sections 3.2, 4.6, 5 and 19, stages MS1a, MS1b and MS3b): the material pin type
// table, the context feature and prerequisite vocabularies, the material node definition format and catalog (built-in nodes and
// plugin registration), the material attribute table, the MaterialGraphSchema domain schemas for material and material function
// documents with the semantic normalization that keeps parameter default values out of the semantic hash, and the device-free
// material value types shared by translation, cooking and runtime (traits, trait overrides, parameter keys and overrides,
// asset references, vertex input usage). No devices, no objects, no lowering code (lowering is MaterialCompiler). Generated
// headers <MaterialSchema/...generated.h>, module registration rocks::generated::MaterialSchema::RegisterModuleTypes (called by
// RegisterMaterialSchemaTypes), identity spec MaterialSchema.rocksids.json.
public sealed class MaterialSchema : EngineModuleRules
{
    protected override string ModuleName => "MaterialSchema";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["NodeGraph", "RenderData", "CoreTypeBindings"];
}
