using Rocks.Build;
using Rocks.Build.Rules;

public sealed class SampleGameModule : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SampleGame",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Source"),
        PrivateIncludeDirectories = [context.RootDirectory, Path.Combine(context.OutputDirectory, "Generated")],
        Dependencies = ["ObjectRuntime"]
    };
}
