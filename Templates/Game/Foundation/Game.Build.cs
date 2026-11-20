using Rocks.Build;
using Rocks.Build.Rules;

public sealed class SampleGameModule : ModuleRules
{
    // Opens the reflection header Source/GameObject.h with the include spelling "GameObject.h". The generated header is <GameObject.generated.h>.
    // The identity spec is SampleGame.rocksids.json in ModuleDirectory (the project root).
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "SampleGame",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Source"),
        PublicIncludeDirectories = [context.PathInRoot("Source")],
        // Launch: the common entry point (crash reporting, command line capture, exit code).
        Dependencies = ["ObjectRuntime", "Launch"],
        Metadata = ["GameObject.h"],
        ModuleDirectory = "."
    };
}
