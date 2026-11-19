using Rocks.Build.Rules;

// Target that builds all engine runtime modules (counterpart of UE's Engine/Source/*.Target.cs). It is the default target of the Installed SDK export,
// and game projects depend on the modules this target builds.
public sealed class EngineTarget : TargetRules
{
    public override string Name => "Engine";

    // CoreTypeBindings: Core value type reflection bindings (Docs/Design/ReflectionFoundations.ko.md section 4.5). It must be in the SDK for game modules
    // to reflect Core value type fields.
    // Launch: the common entry layer every game executable uses (ROCKS_IMPLEMENT_APPLICATION, crash reporting).
    public override IReadOnlyList<string> Modules => ["ObjectSerialization", "Localization", "CoreTypeBindings", "Launch"];

    public override bool CheckPublicHeaders => true;
}
