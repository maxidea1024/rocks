using Rocks.Build;
using Rocks.Build.Rules;

// Input action layer (Docs/Design/InputActions, CR21): input values and keys read from the per-frame input state, the built-in modifiers and
// triggers with their combine rules and trigger events (stage IA1), and later the actions, mapping contexts, the per-player evaluator, user
// rebinding and haptic effects. Mirrors the UE 5.8 Enhanced Input structure with Rocks names. No World or Gameplay dependency: evaluation takes
// an input snapshot and time only.
public sealed class InputActions : EngineModuleRules
{
    protected override string ModuleName => "InputActions";

    protected override UnityMode Unity => UnityMode.On;

    // Input for the input snapshot and key enums, Curves for the user response curve modifier (TangentCurve).
    protected override string[] PublicDependencies => ["Core", "Input", "Curves"];
}
