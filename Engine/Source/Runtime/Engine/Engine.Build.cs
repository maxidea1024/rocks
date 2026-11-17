using Rocks.Build.Rules;

// Engine run state machine, main loop, World execution context and EngineSubsystem. Knows nothing about windows, graphics or gameplay
// (the event pump and rendering connect through EngineLoopHooks, game sessions through the EngineClient extension point).
// Config: the engine borrows the host's config context and runs its sync point before each frame begins.
public sealed class Engine : EngineModuleRules
{
    protected override string ModuleName => "Engine";

    protected override string[] RequiredModules => ["World", "Config"];
}
