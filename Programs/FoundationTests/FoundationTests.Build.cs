using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Rule that bundles engine module tests into one executable. Builds each module's Tests/ folder, the test runner (this folder's
/// Private/) and the test support code (Core/Source/Developer/TestSupport) together.
/// To add tests of a new module, put the module name in the list of its layer (RuntimeTestModules, ObjectTestModules or EngineTestModules).
/// </summary>
public abstract class TestsModule : ModuleRules
{
    /// <summary>Layer included by a test executable. A higher layer includes all tests of the lower layers.</summary>
    protected enum TestLayer { Core, Foundation, Engine }

    // Tests of runtime modules outside the Object family.
    private static readonly string[] RuntimeTestModules = ["Base", "Core", "PlatformBase", "Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "DeterministicMath", "TraceCapture"];

    // Tests of Core layer development tool modules (Core/Source/Developer) (included in every test executable).
    private static readonly string[] DeveloperTestModules = ["CompressionTraining", "TraceAnalysis"];

    // Tests of Object family modules (included in FoundationTests and EngineTests).
    private static readonly string[] ObjectTestModules = ["ObjectRuntime", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig"];

    // Tests of engine layer modules (EngineTests only). Paths are Group/Module under Engine/Source.
    private static readonly string[] EngineTestModules = ["Runtime/Input", "Runtime/PlatformWindow", "Runtime/World", "Runtime/Engine", "Runtime/GraphicsTypes", "Runtime/Graphics", "Runtime/RenderBase", "Runtime/RenderGraph", "Runtime/AssetRuntime", "Developer/AssetTools", "Developer/ShaderToolTable", "Developer/ShaderToolLoader", "Developer/ShaderCompiler", "Developer/ShaderFormatVulkan", "Runtime/VulkanGraphics"];

    // Engine modules EngineTests links (only the top modules tests use; lower dependencies propagate).
    private static readonly string[] EngineTestDependencies = ["PlatformWindow", "Engine", "Graphics", "RenderBase", "RenderGraph", "SpirvReflect", "ShaderToolLoader", "ShaderCompiler", "ShaderFormatVulkan", "VulkanGraphics"];

    // Libraries included by the EngineTests shader tool external library tests (Programs/FoundationTests/Engine). Headers enter only through this executable's
    // Private includes (adapter approach) and must produce zero warnings under the engine warning rules (Docs/Design/ShaderSystem.ko.md section 8).
    private static readonly string[] EngineThirdPartyHeaders = ["VulkanHeaders", "SpirvReflect", "SpirvCross", "SpirvTools"];

    protected abstract TestLayer Layer { get; }

    protected abstract string ExecutableName { get; }

    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        bool objects = Layer != TestLayer.Core;
        var directories = new List<string> { "Programs/FoundationTests/Private" };
        directories.AddRange(RuntimeTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));
        directories.AddRange(DeveloperTestModules.Select(module => $"Core/Source/Developer/{module}/Tests"));
        if (objects)
        {
            directories.AddRange(ObjectTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));
        }

        if (Layer == TestLayer.Engine)
        {
            directories.AddRange(EngineTestModules.Select(module => $"Engine/Source/{module}/Tests")
                .Where(directory => Directory.Exists(context.PathInRoot(directory))));
            directories.Add("Programs/FoundationTests/Engine");
        }

        // POSIX platforms use the shared test support implementation.
        directories.Add(context.Platform == NativePlatform.Windows ? $"{support}/Private/Win64" : $"{support}/Private/Posix");
        var sources = context.Sources(directories.ToArray()).ToList();
        sources.Add(context.PathInRoot(context.Configuration == NativeConfiguration.Asan
            ? $"{support}/Private/Asan/Enabled.cpp" : $"{support}/Private/Asan/Disabled.cpp"));
        string[] dependencies = Layer switch
        {
            TestLayer.Core => ["Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", .. DeveloperTestModules],
            TestLayer.Foundation => ["ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", .. DeveloperTestModules],
            _ => ["ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", .. DeveloperTestModules,
                .. EngineTestDependencies]
        };

        // Object family tests depend on the reflection fixture module (config layer tests on ConfigTestFixtures). Generated headers (<ReflectionTestFixtures/*.generated.h>)
        // propagate through that module's public includes. Engine layer tests also depend on the World reflection fixture (WorldTestFixtures: save and path
        // test types that derive from World types).
        string[] fixtures = Layer switch
        {
            TestLayer.Core => [],
            TestLayer.Foundation => ["ReflectionTestFixtures", "ConfigTestFixtures"],
            _ => ["ReflectionTestFixtures", "ConfigTestFixtures", "WorldTestFixtures"]
        };
        return new()
        {
            Name = ExecutableName,
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            // Core/Source/Runtime: tests include module Private headers with the <Module/Private/...> spelling. The repository root is not included
            // (so headers of gitignored outputs, user directories and nested working copies do not enter the include inventory).
            PrivateIncludeDirectories = [context.PathInRoot("Core/Source/Runtime"), context.PathInRoot($"{support}/Public"),
                .. (Layer == TestLayer.Engine ? ThirdPartyModuleRules.ConsumerIncludeDirectories(context, EngineThirdPartyHeaders) : [])],
            PrivateDefines = ["ROCKS_TESTING=1",
                .. (Layer == TestLayer.Engine ? ThirdPartyModuleRules.ConsumerDefines(context, EngineThirdPartyHeaders) : [])],
            Dependencies = [.. dependencies, .. fixtures]
        };
    }
}

public sealed class FoundationTests : TestsModule
{
    protected override TestLayer Layer => TestLayer.Foundation;

    protected override string ExecutableName => "FoundationTests";
}

public sealed class CoreFoundationTests : TestsModule
{
    protected override TestLayer Layer => TestLayer.Core;

    protected override string ExecutableName => "CoreFoundationTests";
}

public sealed class EngineTests : TestsModule
{
    protected override TestLayer Layer => TestLayer.Engine;

    protected override string ExecutableName => "EngineTests";
}
