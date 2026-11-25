using Rocks.Build;
using Rocks.Build.Rules;

// The Sparks language server (Docs/Design/SparksTools.ko.md section 10, LC8e): `SparksLsp [--stdio] [--schema <folder>]...` speaks
// LSP over the standard streams. The server lives in the SparksTools module; this program only connects it to the standard
// streams through the common entry point (Launch).
public sealed class SparksLspProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SparksLsp",
        ModuleDirectory = "Programs/SparksLsp",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/SparksLsp/Private"),
        PublicDependencies = ["Launch", "Core", "SparksTools"]
    };
}
