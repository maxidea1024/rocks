using Rocks.Build;
using Rocks.Build.Rules;

// Audio cooking and import tools (Docs/Design/AudioSystem.ko.md sections 3.10-3.11, 3.24). The first part is the ADPCM block
// encoder; source importers and the Opus encoder follow. Developer module: editor, cooker and tool programs link it, shipping
// runtime targets do not.
public sealed class AudioTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AudioTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Audio"];
}
