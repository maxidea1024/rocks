using Rocks.Build;
using Rocks.Build.Rules;

// Audio cooking and import tools (Docs/Design/AudioSystem.ko.md sections 3.10-3.11, 3.24-3.26): the ADPCM block encoder and the
// source decoders (WAV, AIFF, MP3); the other source codecs, importers and the Opus encoder follow. Developer module: editor,
// cooker and tool programs link it, shipping runtime targets do not.
public sealed class AudioTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AudioTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Audio"];

    // MP3 source decoding (section 3.26) uses minimp3 through Engine/Source/ThirdParty/Minimp3; its header stays private to this module.
    protected override string[] PrivateDependencies => ["Minimp3"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Minimp3");

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, "Minimp3");
}
