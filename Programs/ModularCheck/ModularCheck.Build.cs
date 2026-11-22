using Rocks.Build;
using Rocks.Build.Rules;

// Modular link check program (Docs/Design/BuildTargets.ko.md section 3.8, BS8d2). Depends on the same runtime, developer and test fixture
// modules as FoundationTests so that a Modular build links every one of them as a shared library and the run goes through the Modular
// executable entry (Docs/Design/ModuleLoader.ko.md section 3.14). Not part of the product runtime.
public sealed class ModularCheckProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "ModularCheck",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/ModularCheck/Private"),
        PublicDependencies =
        [
            "Launch", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression",
            "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", "CompressionTraining", "TraceAnalysis", "GcSoak", "ReflectionTestFixtures",
            "ConfigTestFixtures", "CurvesTestFixtures", "DynamicModuleTestApi"
        ],
        DynamicModules = ["DynamicModuleTestFixture"]
    };
}
