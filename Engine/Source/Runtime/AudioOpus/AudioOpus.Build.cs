using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Runtime Opus decoder for cooked audio platform data (Docs/Design/AudioSystem.ko.md section 3.32.4): an AudioDecoder for codec
// Opus that the decoder table owners register with RegisterOpusAudioDecoder. ClientOnly (AD24, Docs/Design/BuildTargets section 3.3):
// dedicated servers do not decode audio, so Server targets leave it out and Runtime modules must not depend on it.
public sealed class AudioOpus : EngineModuleRules
{
    protected override string ModuleName => "AudioOpus";

    protected override ModuleType ModuleType => ModuleType.ClientOnly;

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Audio"];

    // libopus (float build) from Engine/Source/ThirdParty; its headers stay private to this module.
    protected override string[] PrivateDependencies => ["Opus"];
}
