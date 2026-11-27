using Rocks.Build;
using Rocks.Build.Rules;

// Physics module (Docs/Design/PhysicsModule, stage PH1 and later): the World physics scene implemented with Jolt, the Jolt process
// resources (allocation, trace and assert hooks, version check, factory and type registration), the Jolt job system on the Task worker
// scheduler, the physics settings (branch Engine, section Physics) and the PhysicsWorldSubsystem that plugs the scene into a World. Jolt
// stays private: no Jolt type appears in the public headers (Docs/Design/PhysicsJoltAdapter.ko.md section 10.3). Servers simulate physics,
// so the module is a plain Runtime module (not ClientOnly). Generated headers are <Physics/*.generated.h>, module registration is
// rocks::generated::Physics::RegisterModuleTypes (called by RegisterPhysicsTypes), and the identity spec is Physics.rocksids.json.
public sealed class Physics : EngineModuleRules
{
    protected override string ModuleName => "Physics";

    protected override UnityMode Unity => UnityMode.On;

    // World: the PhysicsScene interface and WorldSubsystem. Collision: channels and body settings the scene maps to Jolt layers.
    protected override string[] PublicDependencies => ["World", "Collision"];

    // Jolt: the solver. Task: the worker scheduler that runs the Jolt jobs.
    // DeterministicMath: the mass power rule (UE RaiseMassToPower) without the host libm, so every platform builds the same body mass.
    protected override string[] PrivateDependencies => ["Jolt", "Task", "DeterministicMath"];
}
