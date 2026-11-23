using Rocks.Build;
using Rocks.Build.Rules;

// World, Actor, Component, SceneComponent, TickScheduler and WorldSubsystem. Does not reference Engine, Graphics or window
// modules (higher modules handle engine execution and render wiring). Level, Actor, Component and SceneComponent are WorldOnly
// reflection classes (Docs/Design/ReflectionFoundations.ko.md section 9.1); generated headers are <World/<Type>.generated.h>,
// module registration is rocks::generated::World::RegisterModuleTypes, declared in <World/World.module.generated.h> and defined in
// the module generated source that the build compiles into this module (called by RegisterWorldTypes), and the identity spec is
// World.rocksids.json.
public sealed class World : EngineModuleRules
{
    protected override string ModuleName => "World";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // CoreTypeBindings: SceneComponent's Transform field imports that module's Binds mirror.
    // Config: a World owns a CVar scope in the host config context (Docs/Design/CVarSystem.ko.md section 14.2).
    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "Config"];
}
