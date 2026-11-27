using Rocks.Build;
using Rocks.Build.Rules;

// Engine run state machine, main loop, World execution context and EngineSubsystem. Knows nothing about windows, graphics or gameplay
// (the event pump and rendering connect through EngineLoopHooks, game sessions through the EngineClient extension point).
// Config: the engine borrows the host's config context and runs its sync point before each frame begins.
// Task: the engine owns the GC marking WorkerScheduler it lends to worlds (Docs/Design/GarbageCollectionScale.ko.md section 17.4).
public sealed class Engine : EngineModuleRules
{
    protected override string ModuleName => "Engine";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // ModuleLoader: the engine attaches its type registrar to the module loader (Docs/Design/ModuleLoader section 3.10). Progress: Engine::Initialize
    // and ModuleStartProgress open progress scopes for the startup splash (Docs/Design/SplashScreen section 8.1); they do nothing without a binding.
    protected override string[] PublicDependencies => ["World", "Config", "Task", "ModuleLoader", "Progress"];
}
