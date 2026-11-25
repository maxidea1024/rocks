using Rocks.Build;
using Rocks.Build.Rules;

// Material graph schema (Docs/Design/MaterialSystem sections 3.2, 5 and 19, stages MS1a and MS1b): the material pin type table, the
// context feature and prerequisite vocabularies, the material node definition format and catalog (built-in nodes and plugin
// registration), the MaterialGraphSchema domain schemas for material and material function documents, and the semantic
// normalization that keeps parameter default values out of the semantic hash. Node property structs are reflected (Record bytes,
// NodeGraph GD23). Engine Runtime module: no devices, no objects, no lowering code (lowering is MaterialCompiler).
// Reflected types arrive in stage MS3b: generated headers <MaterialSchema/...generated.h>, module registration rocks::generated::MaterialSchema::RegisterModuleTypes
// (called by RegisterMaterialSchemaTypes), and the identity spec is MaterialSchema.rocksids.json.
public sealed class MaterialSchema : EngineModuleRules
{
    protected override string ModuleName => "MaterialSchema";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["NodeGraph"];
}
