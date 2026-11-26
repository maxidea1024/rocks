using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Audio output device backend on miniaudio's low-level device API (Docs/Design/AudioSystem section 3.34): MiniaudioOutput implements
// AudioOutputBackend (WASAPI, Core Audio, AAudio or OpenSL ES), and the module lifetime object puts its factory into the host's
// AudioOutputBackendRegistry service. ClientOnly (AD24, Docs/Design/BuildTargets section 3.3): dedicated servers have no audio device, so Server
// targets leave it out and Runtime modules must not depend on it. It is the only Rocks module that uses the Miniaudio third-party library, which has
// process global state (BuildTargets section 3.8 BD29).
public sealed class AudioMiniaudio : EngineModuleRules
{
    protected override string ModuleName => "AudioMiniaudio";

    protected override ModuleType ModuleType => ModuleType.ClientOnly;

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Audio"];

    // miniaudio headers stay private to this module; ModuleLoader for the module lifetime object's host services.
    protected override string[] PrivateDependencies => ["Miniaudio", "ModuleLoader", "PlatformBase"];

    // macOS builds miniaudio with MA_NO_RUNTIME_LINKING (run-time dlopen of the frameworks fails notarization), so the frameworks are linked here.
    public override NativeModuleInput Configure(BuildContext context) =>
        base.Configure(context) with
        {
            Frameworks = context.Platform == NativePlatform.MacOS ? ["CoreFoundation", "CoreAudio", "AudioToolbox"] : []
        };
}
