using Rocks.Build;
using Rocks.Build.Rules;

// Engine side of the audio system (Docs/Design/AudioSystem sections 3.8 and 3.35): AudioEngineSubsystem owns the output device and the
// control pipe, AudioWorldSubsystem and AudioComponent connect Worlds, and the sound asset classes (SoundSource, SoundClip,
// AttenuationSettings) hand immutable shared PCM snapshots to the control side. Runtime module (not ClientOnly): servers link it so game
// code builds unchanged, and without a device every play request is Ignored (section 3.8.4). Device backends are found only through the
// AudioOutputBackendRegistry host service, so this module never depends on AudioMiniaudio. Generated headers are
// <EngineAudio/*.generated.h>, module registration is rocks::generated::EngineAudio::RegisterModuleTypes (called by RegisterEngineAudioTypes),
// and the identity spec is EngineAudio.rocksids.json.
public sealed class EngineAudio : EngineModuleRules
{
    protected override string ModuleName => "EngineAudio";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Engine and World: subsystems and the AudioComponent. AssetRuntime: the sound asset base class. Audio: mixer, device and DSP.
    protected override string[] PublicDependencies => ["Engine", "World", "AssetRuntime", "Audio"];

    // Task: the DedicatedLane that runs the control pipe. ModuleLoader: HostServices lookup of the backend registry and host options.
    // Config: the audio.* CVars.
    protected override string[] PrivateDependencies => ["Task", "ModuleLoader", "Config"];
}
