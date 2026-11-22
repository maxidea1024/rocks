using Rocks.Build;
using Rocks.Build.Rules;

// Material assets (Docs/Design/MaterialSystem.ko.md section 4.2, stage MS3a): the abstract MaterialBase (UE UMaterialInterface) and the
// Material class, without fields yet. Mesh material slots need the reference type before the material schema and compiler exist
// (MeshAnimationAssets.ko.md section 4.3, user decision 2026-11-23). Stage MS3 adds the section 4.2 fields, MaterialInstance and cooking,
// and the MaterialSchema and MaterialRuntime dependencies (section 3.2).
// Generated header is <Material/Material.generated.h>, module registration is rocks::generated::Material::RegisterModuleTypes (called by
// RegisterMaterialTypes), and the identity spec is Material.rocksids.json.
public sealed class Material : EngineModuleRules
{
    protected override string ModuleName => "Material";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["AssetRuntime"];

    protected override IReadOnlyList<string> Metadata => ["Material/Material.h"];
}
