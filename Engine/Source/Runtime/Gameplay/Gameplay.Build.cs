using Rocks.Build;
using Rocks.Build.Rules;

// Gameplay framework (Docs/Design/EngineRuntime sections 18-21): GameInstance, LocalPlayer, their subsystems and the GameInstanceClient
// that implements the Engine's EngineClient extension point. Later stages add GameDirector, GameState, PlayerState, Controller, Pawn and
// CameraDirector (S11, S12). GameInstance and LocalPlayer are reflection classes in the engine registry; generated headers are
// <Gameplay/<Type>.generated.h>, module registration is rocks::generated::Gameplay::RegisterModuleTypes (called by RegisterGameplayTypes),
// and the identity spec is Gameplay.rocksids.json.
public sealed class Gameplay : EngineModuleRules
{
    protected override string ModuleName => "Gameplay";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Input: a LocalPlayer reads the host's frame input state (section 18.4).
    protected override string[] PublicDependencies => ["Engine", "World", "Input"];
}
