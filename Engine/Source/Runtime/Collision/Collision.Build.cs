using Rocks.Build;
using Rocks.Build.Rules;

// Collision data shared by components, mesh assets and the physics module (Docs/Design/CollisionData.ko.md, stage CD1): collision channels,
// responses and the collision-enabled state (UE ECollisionChannel, ECollisionResponse, ECollisionEnabled), the per-channel response set, the
// collision profile settings (branch Engine, section Collision — UE UCollisionProfile), the profile table built from them, collision
// resolution, and the body settings struct (UE FBodyInstance persistent fields). Data only: no physics engine. World (PrimitiveComponent)
// and Mesh (simple collision) both depend on it, so the types are defined once below both (World does not depend on asset modules).
// Engine Runtime module: ObjectRuntime for reflection, CoreTypeBindings for the Vec3d mirror and Config for the settings struct. Generated
// headers are <Collision/*.generated.h>, module registration is rocks::generated::Collision::RegisterModuleTypes (called by
// RegisterCollisionTypes), and the identity spec is Collision.rocksids.json.
public sealed class Collision : EngineModuleRules
{
    protected override string ModuleName => "Collision";

    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "Config"];
}
