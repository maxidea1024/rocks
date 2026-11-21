using Rocks.Build;
using Rocks.Build.Rules;

public sealed class FoundationTemplateModule : ModuleRules
{
    // Opens the reflection header Source/GameObject.h with the include spelling "GameObject.h". The generated header is <GameObject.generated.h>.
    // The identity spec is FoundationTemplate.rocksids.json in ModuleDirectory (the project root).
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "FoundationTemplate",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Source"),
        PublicIncludeDirectories = [context.PathInRoot("Source")],
        // Launch: the common entry point (crash reporting, command line capture, exit code).
        PublicDependencies = ["ObjectRuntime", "Launch"],
        Metadata = ["GameObject.h"],
        ModuleDirectory = "."
    };
}
