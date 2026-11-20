using Rocks.Build;
using Rocks.Build.Rules;

// zstd dictionary training (tool only, Docs/Design/Compression.ko.md section 11.6). zstd dictBuilder calls the CRT malloc directly (an
// approved narrow exception: training time only), so cookers and tools link it, not the game runtime. This is the only module that includes zdict.h.
public sealed class CompressionTraining : CoreModuleRules
{
    private static readonly string[] Codecs = ["Zstd"];

    protected override string Group => "Developer";

    protected override string ModuleName => "CompressionTraining";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Compression", .. Codecs];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, Codecs);

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, Codecs);
}
