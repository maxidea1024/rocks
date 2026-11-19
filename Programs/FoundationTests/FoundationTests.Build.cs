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
    private static readonly string[] RuntimeTestModules = ["Base", "Core", "PlatformBase", "Launch", "Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "DeterministicMath", "TraceCapture", "ModuleLoader"];

    // Tests of Core layer development tool modules (Core/Source/Developer) (included in every test executable).
    private static readonly string[] DeveloperTestModules = ["CompressionTraining", "TraceAnalysis"];

    // Development tool modules the Object family tests use (FoundationTests and EngineTests link them). GcSoak: long-running GC smoke simulator.
    private static readonly string[] ObjectDeveloperModules = ["GcSoak"];

    // Tests of Object family modules (included in FoundationTests and EngineTests).
    private static readonly string[] ObjectTestModules = ["ObjectRuntime", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Curves"];

    // Tests of engine layer modules (EngineTests only). Paths are Group/Module under Engine/Source.
    private static readonly string[] EngineTestModules = ["Runtime/Input", "Runtime/PlatformWindow", "Runtime/World", "Runtime/Engine", "Runtime/GraphicsTypes", "Runtime/Graphics", "Runtime/RenderBase", "Runtime/RenderGraph", "Runtime/AssetRuntime", "Developer/AssetTools", "Developer/ShaderToolTable", "Developer/ShaderToolLoader", "Developer/ShaderCompiler", "Developer/ShaderFormatVulkan", "Runtime/VulkanGraphics", "Runtime/SpatialIndex"];

    // Engine modules EngineTests links (only the top modules tests use; lower dependencies propagate).
    private static readonly string[] EngineTestDependencies = ["PlatformWindow", "Engine", "Graphics", "RenderBase", "RenderGraph", "SpirvReflect", "ShaderToolLoader", "ShaderCompiler", "ShaderFormatVulkan", "VulkanGraphics", "SpatialIndex"];

    // Libraries included by the EngineTests shader tool external library tests (Programs/FoundationTests/Engine). Headers enter only through this executable's
    // Private includes (adapter approach) and must produce zero warnings under the engine warning rules (Docs/Design/ShaderSystem.ko.md section 8).
    private static readonly string[] EngineThirdPartyHeaders = ["VulkanHeaders", "SpirvReflect", "SpirvCross", "SpirvTools"];

    // Unity build (Docs/Design/UnityBuildPch.ko.md section 9.3). Test sources keep their file-local helpers and TEST bodies in a namespace
    // unique to the file (<file>_file), so helpers with the same name in two sources never meet in one batch. The sources below stay standalone
    // translation units: their helpers are used from another namespace of the same file (specializations of rocks templates for test types,
    // host service macros, probes called from the runner), so they cannot be enclosed mechanically.
    private static readonly string[] UnityStandaloneSources =
    [
        "Core/Source/Runtime/Base/Tests/BaseTests.cpp",
        "Core/Source/Runtime/Config/Tests/ConsoleInputTests.cpp",
        "Core/Source/Runtime/Core/Tests/BinnedAllocatorFailureControls.cpp",
        "Core/Source/Runtime/Core/Tests/LogSystemTests.cpp",
        "Core/Source/Runtime/Core/Tests/LogTemplateLocationProbe.cpp",
        "Core/Source/Runtime/Core/Tests/LogTemplateTests.cpp",
        "Core/Source/Runtime/Core/Tests/MemoryAllocatorEdgeTests.cpp",
        "Core/Source/Runtime/Core/Tests/OperatorNewCounter.cpp",
        "Core/Source/Runtime/Core/Tests/StackAllocatorTests.cpp",
        "Core/Source/Runtime/Core/Tests/StringFormatTests.cpp",
        "Core/Source/Runtime/ModuleLoader/Tests/ModuleLoaderFailureControls.cpp",
        "Core/Source/Runtime/ModuleLoader/Tests/ModuleLoaderTests.cpp",
        "Core/Source/Runtime/ObjectRuntime/Tests/DestroyRequestedFailureTests.cpp",
        "Core/Source/Runtime/ObjectRuntime/Tests/ObjectDiscardTests.cpp",
        "Core/Source/Runtime/ObjectRuntime/Tests/Reflection/PropertyPathTests.cpp",
        "Core/Source/Runtime/ObjectRuntime/Tests/Reflection/RequiredCheckContextTests.cpp",
        "Core/Source/Runtime/ObjectRuntime/Tests/TypeConvenienceTests.cpp",
        "Core/Source/Runtime/ObjectSerialization/Tests/ArrayGraphTests.cpp",
        "Core/Source/Runtime/ObjectSerialization/Tests/StructGraphTests.cpp",
        "Core/Source/Runtime/PlatformBase/Tests/FileFailureControls.cpp",
        "Core/Source/Runtime/PlatformBase/Tests/ProcessProbe.cpp",
        "Engine/Source/Runtime/RenderBase/Tests/GpuTraceTests.cpp",
        "Engine/Source/Runtime/RenderBase/Tests/RenderLaneTests.cpp",
        "Engine/Source/Runtime/RenderBase/Tests/RenderSettingsTests.cpp",
        "Engine/Source/Runtime/RenderGraph/Tests/RenderGraphLifecycleTests.cpp",
        "Engine/Source/Runtime/RenderGraph/Tests/RenderGraphReuseTests.cpp",
        "Engine/Source/Runtime/RenderGraph/Tests/RenderGraphSharedDataTests.cpp",
        "Engine/Source/Runtime/World/Tests/WorldReflectionTests.cpp",
        "Programs/FoundationTests/Private/LoggerProcessProbe.cpp",
        "Programs/FoundationTests/Private/NumberCorpus.cpp",
    ];

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
            TestLayer.Core => ["Launch", "Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", .. DeveloperTestModules],
            TestLayer.Foundation => ["Launch", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", .. DeveloperTestModules, .. ObjectDeveloperModules],
            _ => ["Launch", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", .. DeveloperTestModules, .. ObjectDeveloperModules,
                .. EngineTestDependencies]
        };

        // Object family tests depend on the reflection fixture module (config layer tests on ConfigTestFixtures, curve tests on CurvesTestFixtures). Generated headers (<ReflectionTestFixtures/*.generated.h>)
        // propagate through that module's public includes. Engine layer tests also depend on the World reflection fixture (WorldTestFixtures: save and path
        // test types that derive from World types) and the asset fixture (AssetTestFixtures: test asset classes that derive from AssetRuntime's Asset).
        string[] fixtures = Layer switch
        {
            TestLayer.Core => [],
            TestLayer.Foundation => ["ReflectionTestFixtures", "ConfigTestFixtures", "CurvesTestFixtures"],
            _ => ["ReflectionTestFixtures", "ConfigTestFixtures", "CurvesTestFixtures", "WorldTestFixtures", "AssetTestFixtures"]
        };
        return new()
        {
            Name = ExecutableName,
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            Unity = RepositoryUnity.Effective(UnityMode.On, context),
            UnityExclude = UnityStandaloneSources.Select(context.PathInRoot).ToArray(),
            // Core/Source/Runtime (and Engine/Source/Runtime for the engine layer): tests include module Private headers with the
            // <Module/Private/...> spelling. The repository root is not included (so headers of gitignored outputs, user directories and
            // nested working copies do not enter the include inventory).
            PrivateIncludeDirectories = [context.PathInRoot("Core/Source/Runtime"), context.PathInRoot($"{support}/Public"),
                .. (Layer == TestLayer.Engine ? [context.PathInRoot("Engine/Source/Runtime")] : Array.Empty<string>()),
                .. (Layer == TestLayer.Engine ? ThirdPartyModuleRules.ConsumerIncludeDirectories(context, EngineThirdPartyHeaders) : [])],
            // FOUNDATION_TEST_RUNNER_LAYER picks the runner name in Private/Main.cpp (0 Core, 1 Foundation, 2 Engine).
            PrivateDefines = ["ROCKS_TESTING=1", $"FOUNDATION_TEST_RUNNER_LAYER={(int)Layer}",
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
