using Rocks.Build;
using Rocks.Build.Rules;

// Components that reference assets, above World and the asset modules (Docs/Design/EngineRuntime section 3 and chapter 30, D2b): the mesh
// components (MeshComponent, StaticMeshComponent) and StaticMeshActor. World objects reference assets through SoftObjectRef (section 30.1 —
// World and asset objects live in different registries); mesh loading and RenderScene extraction come later. Textures/ converts cooked
// texture platform data into RenderBase cooked texture uploads (PlatformTextureSource) and owns one GPU texture per texture asset
// (TextureRenderCache — Docs/Design/RenderBase section 18.5 item 8, SM8). Generated headers are <WorldIntegration/<Folder>/<Type>.generated.h>, module registration is
// rocks::generated::WorldIntegration::RegisterModuleTypes (called by RegisterWorldIntegrationTypes, which also registers the World, Mesh and
// Material types), and the identity spec is WorldIntegration.rocksids.json.
public sealed class WorldIntegration : EngineModuleRules
{
    protected override string ModuleName => "WorldIntegration";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // World: the component and actor bases. Mesh: StaticMesh (the expected type of the mesh reference and the loaded mesh bounds).
    // Material: MaterialBase (the expected type of the material references). Texture: Texture and TexturePlatformData (the cooked data the
    // texture render cache uploads; AssetRuntime comes through it). RenderBase: the render lane, render handles and cooked texture uploads.
    protected override string[] PublicDependencies => ["World", "Mesh", "Material", "Texture", "RenderBase"];
}
