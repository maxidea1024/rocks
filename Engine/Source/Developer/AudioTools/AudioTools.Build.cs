using Rocks.Build;
using Rocks.Build.Rules;

// Audio cooking and import tools (Docs/Design/AudioSystem.ko.md sections 3.10-3.11, 3.24-3.29): the ADPCM block encoder and the
// source decoders (WAV, AIFF, MP3, FLAC, Vorbis, Opus); importers and the Opus encoder follow. Developer module:
// editor, cooker and tool programs link it, shipping runtime targets do not.
public sealed class AudioTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AudioTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Audio"];

    // Source decoders from Engine/Source/ThirdParty (sections 3.26-3.29): minimp3 for MP3, libFLAC for FLAC, libvorbis for Ogg Vorbis,
    // opusfile and libopus for Ogg Opus, libogg for both Ogg formats. Their headers stay private to this module. The Ogg users come
    // before Ogg (and OpusFile before Opus) so that a left-to-right static linker resolves their references.
    protected override string[] PrivateDependencies => ["Minimp3", "Flac", "Vorbis", "OpusFile", "Opus", "Ogg"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Minimp3", "Flac", "Vorbis", "OpusFile", "Opus", "Ogg");

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, "Minimp3", "Flac", "Vorbis", "OpusFile", "Opus", "Ogg");
}
