using Rocks.Build;
using Rocks.Build.Rules;

// The Sparks command line tool (Docs/Design/SparksTools.ko.md, LC8): `SparksCli check|dump ...`. The commands live in the SparksTools
// module; this program only connects them to the standard streams through the common entry point (Launch).
public sealed class SparksCliProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SparksCli",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/SparksCli/Private"),
        PublicDependencies = ["Launch", "Core", "SparksTools"]
    };
}
