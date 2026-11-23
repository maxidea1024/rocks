using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compile worker program (Docs/Design/ShaderSystem.ko.md section 11 and 11.1 SC4-SC7, SC14): the Launch entry point around ShaderWorker.
/// The main process starts it next to its own executable (companion target). Crash reporting writes in-process dumps under the work directory and
/// system error dialogs are suppressed (a worker must never block on a dialog).
/// </summary>
public sealed class ShaderCompileWorkerProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "ShaderCompileWorker",
        ModuleDirectory = "Programs/ShaderCompileWorker",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/ShaderCompileWorker/Private"),
        PublicDependencies = ["Core", "Launch", "ShaderWorker"]
    };
}
