using Rocks.Build;
using Rocks.Build.Rules;

// Material assets (Docs/Design/MaterialSystem sections 4.2, 4.6 and 19.9 to 19.11, stages MS3a and MS3b): MaterialBase (UE
// UMaterialInterface), Material, MaterialInstance, MaterialFunction, MaterialFunctionInstance and ParameterCollection with their
// fields, and the asset-side value types that hold object references (texture parameter overrides, layer stacks). Cooking data
// (translation results) arrives in stage MS3. Generated headers <Material/...generated.h>, module registration
// rocks::generated::Material::RegisterModuleTypes (called by RegisterMaterialTypes), identity spec Material.rocksids.json.
public sealed class Material : EngineModuleRules
{
    protected override string ModuleName => "Material";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["AssetRuntime", "MaterialSchema", "Texture"];
}
