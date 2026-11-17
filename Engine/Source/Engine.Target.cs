using Rocks.Build.Rules;

// Target that builds all engine runtime modules (counterpart of UE's Engine/Source/*.Target.cs). It is the default target of the Installed SDK export,
// and game projects depend on the modules this target builds.
public sealed class EngineTarget : TargetRules
{
    public override string Name => "Engine";

    // CoreTypeBindings: Core value type reflection bindings (Docs/Design/ReflectionFoundations.ko.md section 4.5). It must be in the SDK for game modules
    // to reflect Core value type fields.
    public override IReadOnlyList<string> Modules => ["ObjectSerialization", "Localization", "CoreTypeBindings"];

    public override bool CheckPublicHeaders => true;
}
