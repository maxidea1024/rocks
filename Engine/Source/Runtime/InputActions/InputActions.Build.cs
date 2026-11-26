using Rocks.Build;
using Rocks.Build.Rules;

// Input action layer (Docs/Design/InputActions, CR21): input values and keys read from the per-frame input state, the built-in
// modifiers and triggers with their combine rules and trigger events (stage IA1), the reflected action and mapping context data
// with collection into evaluator bundles and validation (stage IA2a), and later the per-player evaluator, user rebinding and
// haptic effects. Mirrors the UE 5.8 Enhanced Input structure with Rocks names. No World or Gameplay dependency: evaluation takes
// an input snapshot and time only.
public sealed class InputActions : EngineModuleRules
{
    protected override string ModuleName => "InputActions";

    protected override UnityMode Unity => UnityMode.On;

    // Input for the input snapshot and key enums, Curves for the user response curve modifier (TangentCurve), ObjectRuntime for the
    // InputAction and InputContext classes, CoreTypeBindings for the Vec3d mirror, Localization for display Text and AssetRuntime
    // for the Asset base of the input asset classes and the lease readers (stage IA6a — dedicated servers read action assets too).
    // Generated headers are <InputActions/<Header>.generated.h>, module registration is
    // rocks::generated::InputActions::RegisterModuleTypes (called by RegisterInputActionsTypes), and the identity spec is
    // InputActions.rocksids.json.
    protected override string[] PublicDependencies => ["Core", "Input", "Curves", "ObjectRuntime", "CoreTypeBindings", "Localization", "AssetRuntime"];

    // Config: the generated module registration also registers the reflected types of Input's private dependency Config.
    protected override string[] PrivateDependencies => ["Config"];
}
