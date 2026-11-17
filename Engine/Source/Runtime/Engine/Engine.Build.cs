using Rocks.Build.Rules;

// Engine run state machine, main loop, World execution context and EngineSubsystem. Knows nothing about windows, graphics or gameplay
// (the event pump and rendering connect through EngineLoopHooks, game sessions through the EngineClient extension point).
public sealed class Engine : EngineModuleRules
{
    protected override string ModuleName => "Engine";

    protected override string[] RequiredModules => ["World"];
}
