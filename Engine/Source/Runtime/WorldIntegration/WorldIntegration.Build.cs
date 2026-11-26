using Rocks.Build;
using Rocks.Build.Rules;

// Components that reference assets, above World and the asset modules (Docs/Design/EngineRuntime section 3 and chapter 30, D2b): the mesh
// components (MeshComponent, StaticMeshComponent) and StaticMeshActor. World objects reference assets through SoftObjectRef (section 30.1 —
// World and asset objects live in different registries); loading and render wiring (RenderScene extraction) come later and add RenderBase
// then. Generated headers are <WorldIntegration/<Folder>/<Type>.generated.h>, module registration is
// rocks::generated::WorldIntegration::RegisterModuleTypes (called by RegisterWorldIntegrationTypes, which also registers the World, Mesh and
// Material types), and the identity spec is WorldIntegration.rocksids.json.
public sealed class WorldIntegration : EngineModuleRules
{
    protected override string ModuleName => "WorldIntegration";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // World: the component and actor bases. Mesh: StaticMesh (the expected type of the mesh reference and the loaded mesh bounds).
    // Material: MaterialBase (the expected type of the material references).
    protected override string[] PublicDependencies => ["World", "Mesh", "Material"];
}
