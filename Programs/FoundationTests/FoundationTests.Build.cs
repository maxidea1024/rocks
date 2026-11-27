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
    private static readonly string[] RuntimeTestModules = ["Base", "Core", "PlatformBase", "Launch", "Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "DeterministicMath", "TraceCapture", "ModuleLoader", "Progress"];

    // Tests of Core layer development tool modules (Core/Source/Developer) (included in every test executable).
    private static readonly string[] DeveloperTestModules = ["CompressionTraining", "TraceAnalysis"];

    // Development tool modules the Object family tests use (FoundationTests and EngineTests link them). GcSoak: long-running GC smoke simulator.
    private static readonly string[] ObjectDeveloperModules = ["GcSoak"];

    // Tests of Object family modules (included in FoundationTests and EngineTests).
    private static readonly string[] ObjectTestModules = ["MetaSchema", "ObjectRuntime", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Curves"];

    // Tests of engine layer modules (EngineTests only). Paths are Group/Module under Engine/Source.
    private static readonly string[] EngineTestModules = ["Runtime/Input", "Runtime/InputActions", "Runtime/CurveAssets", "Developer/CurveAssetTools", "Runtime/PlatformWindow", "Runtime/Collision", "Runtime/World", "Runtime/WorldIntegration", "Runtime/Physics", "Runtime/Engine", "Runtime/Gameplay", "Runtime/GraphicsTypes", "Runtime/Graphics", "Runtime/RenderBase", "Runtime/RenderGraph", "Runtime/AssetRuntime", "Runtime/EngineAssets", "Developer/AssetDatabase", "Developer/AssetCommandlets", "Developer/AssetNugget", "Developer/AssetCooker", "Developer/TargetProfiles", "Developer/AssetTools", "Runtime/Texture", "Developer/ImageCodec", "Developer/TextureTools", "Runtime/ShaderCore", "Developer/ShaderToolTable", "Developer/ShaderToolLoader", "Developer/ShaderCompilerCommon", "Developer/ShaderCompiler", "Developer/ShaderFormatVulkan", "Runtime/VulkanGraphics", "Runtime/SpatialIndex", "Runtime/MeshSource", "Runtime/Mesh", "Developer/MeshTools", "Runtime/Animation", "Developer/AnimationTools", "Runtime/Material", "Runtime/MaterialSchema", "Developer/MaterialCompiler", "Runtime/SparksVm", "Runtime/SparksCompiler", "Developer/SparksTestHost", "Developer/SparksTools", "Runtime/Audio", "Runtime/AudioMiniaudio", "Runtime/EngineAudio", "Developer/AudioTools", "Runtime/NodeGraph", "Developer/NodeGraphTools", "Runtime/TextLayout", "Runtime/TextLayoutBackend", "Runtime/SplashScreen"];

    // Engine plugin modules whose tests EngineTests runs (Docs/Design/BuildTargets section 3.9 BD40), as "<plugin path under Engine/Plugins>/<module>".
    // The tests live in <plugin>/Source/<module>/Tests/ and include module Private headers as <Module/Private/...> through <plugin>/Source. The
    // EngineTests target enables these plugins (EngineTestsTarget.Plugins); EngineTests links their modules.
    private static readonly string[] PluginTestModules = ["Importers/UnrealExchange/UnrealExchange"];

    // Engine modules EngineTests links (only the top modules tests use; lower dependencies propagate).
    private static readonly string[] EngineTestDependencies = ["PlatformWindow", "Physics", "InputActions", "CurveAssets", "CurveAssetTools", "Engine", "Gameplay", "WorldIntegration", "Graphics", "RenderBase", "RenderGraph", "ShaderCore", "ShaderToolLoader", "ShaderCompiler", "ShaderFormatVulkan", "VulkanGraphics", "SpatialIndex", "MeshSource", "MeshTools", "Animation", "AnimationTools", "Material", "MaterialSchema", "MaterialCompiler", "Texture", "ImageCodec", "TextureTools", "SparksTestHost", "SparksTools", "EngineAssets", "AssetDatabase", "AssetCommandlets", "AssetNugget", "AssetCooker", "TargetProfiles", "Audio", "AudioOpus", "AudioMiniaudio", "EngineAudio", "AudioTools", "NodeGraph", "NodeGraphTools", "TextLayoutBackend", "SplashScreen"];

    // Libraries included by the EngineTests shader tool external library tests (Programs/FoundationTests/Engine), the MeshTools MikkTSpace reference
    // comparison test, the font library tests (Docs/Design/Font section 8.5, FT1), the Jolt physics link test (Docs/Design/PhysicsJoltAdapter.ko.md
    // section 10.5, JL1) and the ImageCodec JPEG tests that write fixtures through libjpeg. Headers enter only through this executable's Private
    // includes (adapter approach) and must produce zero warnings under the engine warning rules (Docs/Design/ShaderSystem section 8). libtiff and
    // OpenEXRCore have process-wide state, so only ImageCodec depends on them (BD29) and the TIFF and EXR tests write fixtures through the ImageCodec
    // encode API (Docs/Design/AssetSystem section 12.10.1).
    private static readonly string[] EngineThirdPartyHeaders = ["VulkanHeaders", "SpirvReflect", "SpirvCross", "SpirvTools", "MikkTSpace", "FreeType", "LibPng", "Msdfgen", "Jolt", "LibJpegTurbo"];

    // Third-party libraries with process-wide state (ThirdParty.json globalState) among EngineThirdPartyHeaders, and the test sources that use them
    // directly. Physics owns Jolt; only the Monolithic engine runner may consume it beside its owner (GlobalStateTestConsumer — Docs/Design/BuildTargets
    // section 3.8 BD29 test executable exception, revision 38). The Modular runner leaves the library off and skips these sources (the Monolithic runner
    // runs them).
    private static readonly string[] GlobalStateThirdPartyHeaders = ["Jolt"];

    private static readonly string[] GlobalStateDirectSources =
    [
        "Programs/FoundationTests/Engine/ThirdPartyJoltTests.cpp",
        "Engine/Source/Runtime/Physics/Tests/PhysicsAdapterTests.cpp",
        "Engine/Source/Runtime/Physics/Tests/PhysicsContactSceneTests.cpp",
        "Engine/Source/Runtime/Physics/Tests/PhysicsSceneTests.cpp"
    ];

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
        "Core/Source/Runtime/MetaSchema/Tests/LaneTransferTests.cpp",
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
        // Jolt headers bring STL headers and std names into the JPH namespace; keep them out of the unity batches of other tests.
        "Programs/FoundationTests/Engine/ThirdPartyJoltTests.cpp",
        "Programs/FoundationTests/Private/LoggerProcessProbe.cpp",
        "Programs/FoundationTests/Private/NumberCorpus.cpp",
    ];

    // Modules the tests of a module may use besides that module's compile environment (Docs/Design/BuildTargets section 3.5 BS1b): fixture modules,
    // test-only tools and modules an integration test exercises. The include ownership check lets the tests in <module>/Tests include only headers of
    // the module, the public and Tests areas of the modules its compile environment sees, and the public and Tests areas of these modules (with the
    // public areas of their public environment). Keep each list to what the tests include.
    private static readonly Dictionary<string, string[]> TestGroupModules = new(StringComparer.Ordinal)
    {
        ["Animation"] = ["AssetTestFixtures"],
        ["AnimationTools"] = ["AssetTestFixtures"],
        ["AssetCommandlets"] = ["AssetTestFixtures", "CoreTypeBindings", "Curves"],
        ["AssetCooker"] = ["AssetTestFixtures", "CoreTypeBindings", "Curves"],
        ["AssetDatabase"] = ["AssetTestFixtures", "CoreTypeBindings", "Curves"],
        ["AssetNugget"] = ["AssetDatabase", "AssetRuntime", "AssetTestFixtures", "ObjectSerialization"],
        // The package event binding tests put the reflection fixture's event publisher and call target in a package (Docs/Design/EventReflection EV3a2).
        ["AssetRuntime"] = ["AssetTestFixtures", "CoreTypeBindings", "Curves", "ReflectionTestFixtures"],
        // The Opus codec tests round-trip the AudioTools encoder through the AudioOpus decoder (AudioTools itself does not depend on it).
        ["AudioTools"] = ["AudioOpus"],
        ["Base"] = ["Core"],
        ["Compression"] = ["AsyncCompression", "Task"],
        ["CompressionTraining"] = ["Task"],
        ["Collision"] = ["ConfigTestFixtures"],
        ["Config"] = ["ConfigTestFixtures", "CoreTypeBindings"],
        // The segment stream negative tests compress their payloads.
        ["Crypto"] = ["Compression"],
        ["Curves"] = ["CoreTypeBindings", "CurvesTestFixtures", "ObjectSerialization", "ReflectionText"],
        // The curve source tests hold the sources in a reflected fixture asset (CurveAssetsTestFixtures — reference structs live only in owners).
        ["CurveAssets"] = ["CurveAssetsTestFixtures"],
        // The curve table cook tests write and load packages with the asset test support (AssetTestSupport.h uses the fixture types).
        ["CurveAssetTools"] = ["AssetTestFixtures"],
        ["Engine"] = ["ConfigTestFixtures", "WorldTestFixtures"],
        // Gameplay diagnostics tests drive the console commands through the Config test fixture context; the input asset tests
        // (InputActions IA6b) cook input context packages with the asset test scene support.
        ["Gameplay"] = ["AssetTestFixtures", "ConfigTestFixtures"],
        ["EngineAssets"] = ["AssetTestFixtures", "ConfigTestFixtures", "Curves"],
        // The EngineAudio settings test builds an engine on the Config test fixture context (audio.* CVars).
        ["EngineAudio"] = ["ConfigTestFixtures", "AudioTools", "AudioOpus"],
        ["ImageCodec"] = ["Compression"],
        // Input asset tests (InputActions IA6a) cook input asset packages with the asset test scene support; the function binding tests
        // (IA7) bind to the reflected handlers of InputActionsTestFixtures.
        ["InputActions"] = ["AssetTestFixtures", "InputActionsTestFixtures"],
        // MemoryFileSystem.h(AssetDatabase Tests — header only): the profile tests read layered JSON from memory.
        ["TargetProfiles"] = ["AssetDatabase"],
        ["Texture"] = ["ObjectSerialization"],
        ["TextureTools"] = ["AssetTestFixtures", "Compression", "CoreTypeBindings", "Curves"],
        ["Material"] = ["AssetTestFixtures"],
        // The dynamic pin tests write node properties with the reflection codec (SaveStructRecord) before reading them back as a node would.
        ["MaterialSchema"] = ["ObjectSerialization"],
        // The translator tests build material graphs whose node properties and pin values are written with the reflection codec.
        ["MaterialCompiler"] = ["ObjectSerialization"],
        ["Mesh"] = ["AssetTestFixtures", "MeshSource"],
        ["MeshSource"] = ["AssetTestFixtures", "CoreTypeBindings", "Mesh"],
        ["MeshTools"] = ["AssetTestFixtures", "Compression", "CoreTypeBindings", "Curves"],
        // The lane transfer tests check the reflected fixture structs (Object references, weak references) against the schema rule.
        ["MetaSchema"] = ["ReflectionTestFixtures"],
        ["ModuleLoader"] = ["DynamicModuleTestApi"],
        ["NodeGraph"] = ["ReflectionText"],
        ["NodeGraphTools"] = ["ReflectionTestFixtures"],
        ["ObjectRuntime"] = ["CoreTypeBindings", "GcSoak", "ObjectSerialization", "ReflectionTestFixtures"],
        ["ObjectSerialization"] = ["CoreTypeBindings", "ReflectionTestFixtures"],
        ["Physics"] = ["ConfigTestFixtures"],
        ["PlatformBase"] = ["Core"],
        ["ReflectionText"] = ["CoreTypeBindings", "ReflectionTestFixtures"],
        // The RenderBase tests also cover its integration with the engine (render settings through Config, GPU trace through TraceAnalysis, the
        // World render lane) and build resource maps from the generated shader parameter fixture structs (Docs/Design/ShaderSystem section 16.2 SSD64).
        ["RenderBase"] = ["Config", "ConfigTestFixtures", "CoreTypeBindings", "Engine", "ObjectRuntime", "ReflectionTestFixtures", "ReflectionText",
            "Serialization", "ShaderParameterTestFixtures", "TraceAnalysis", "World"],
        // The render graph parameter collector tests add passes from the generated shader parameter fixture structs (Docs/Design/ShaderSystem section
        // 16.2 SSD65).
        ["RenderGraph"] = ["ShaderParameterTestFixtures"],
        // The ShaderToolLoader tests drive the loaded tools through their C headers.
        ["ShaderToolLoader"] = ["SpirvCross", "SpirvTools"],
        // The engine shader compile matrix (Docs/Design/EngineShaders section 7.6) compiles the engine shader types RenderBase registers; the shader
        // parameter struct tests compile the generated fixture structs (Docs/Design/ShaderSystem section 16.2 SSD58).
        ["ShaderCompiler"] = ["DeterministicMath", "RenderBase", "ShaderParameterTestFixtures"],
        ["SparksCompiler"] = ["Config", "HostConfig", "MetaSchema", "ModuleLoader", "ObjectRuntime", "ReflectionText"],
        ["SparksTestHost"] = ["Config", "HostConfig", "MetaSchema", "ModuleLoader", "ObjectRuntime", "ReflectionText"],
        ["SparksTools"] = ["Config", "HostConfig", "MetaSchema", "ModuleLoader", "ObjectRuntime", "ReflectionText"],
        ["Task"] = ["TraceAnalysis"],
        ["Texture"] = ["AssetTestFixtures", "ObjectSerialization"],
        ["TraceCapture"] = ["TraceAnalysis"],
        ["TraceControl"] = ["ConfigTestFixtures", "CoreTypeBindings", "TraceAnalysis"],
        ["UnrealExchange"] = ["Animation", "AssetTestFixtures", "CoreTypeBindings", "Curves", "Mesh", "MeshTools"],
        // The World collision tests build the profile table from a Config test fixture context (Docs/Design/CollisionData.ko.md CD2).
        ["World"] = ["ConfigTestFixtures", "ObjectSerialization", "Task", "WorldTestFixtures"],
        // The texture render cache tests load cooked texture packages through the AssetRuntime loader test rig (Docs/Design/RenderBase section
        // 18.5 item 8).
        ["WorldIntegration"] = ["AssetTestFixtures"]
    };

    // Modules whose private headers (test access headers) the tests of a module include (Docs/Design/BuildTargets section 3.5 BS1b). Each entry is
    // an explicit test access to another module's internals.
    private static readonly Dictionary<string, string[]> TestGroupInternals = new(StringComparer.Ordinal)
    {
        // PackageInternal.h: the package writer internals the asset tests build packages with.
        ["AssetCooker"] = ["AssetRuntime"],
        ["AssetDatabase"] = ["AssetRuntime"],
        ["EngineAssets"] = ["AssetRuntime"],
        ["Gameplay"] = ["AssetRuntime"],
        ["InputActions"] = ["AssetRuntime"],
        ["WorldIntegration"] = ["AssetRuntime"],
        // VirtualMemoryTestAccess.h: the access overlap checker tests drive the virtual memory backend.
        ["Core"] = ["PlatformBase"]
    };

    // <plugin>/Source of a PluginTestModules entry ("Importers/UnrealExchange/UnrealExchange" -> "Engine/Plugins/Importers/UnrealExchange/Source").
    private static string PluginSourceDirectory(string module) => $"Engine/Plugins/{module[..module.LastIndexOf('/')]}/Source";

    // Module name of a PluginTestModules entry ("Importers/UnrealExchange/UnrealExchange" -> "UnrealExchange").
    private static string PluginModuleName(string module) => module[(module.LastIndexOf('/') + 1)..];

    protected abstract TestLayer Layer { get; }

    protected abstract string ExecutableName { get; }

    // Whether this runner belongs to a Modular target (every library module a shared library).
    protected virtual bool Modular => false;

    // Whether the runner also runs the PluginTestModules tests (and links those plugin modules): both engine layer runners do. The Modular one
    // loads the plugin modules from their plugin binaries folders (Docs/Design/ModuleLoader section 3.15 ML5a).
    private bool PluginTests => Layer == TestLayer.Engine;

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
            if (PluginTests)
            {
                directories.AddRange(PluginTestModules.Select(module => $"{PluginSourceDirectory(module)}/{PluginModuleName(module)}/Tests"));
            }

            directories.Add("Programs/FoundationTests/Engine");
        }

        // One source group per tested module (Docs/Design/BuildTargets section 3.5 BS1b): the folder of its tests, the module, and its extra modules.
        SourceGroup[] groups = [.. directories.Where(directory => directory.EndsWith("/Tests", StringComparison.Ordinal))
            .Select(directory =>
            {
                string target = directory[..^"/Tests".Length];
                target = target[(target.LastIndexOf('/') + 1)..];
                return new SourceGroup(directory, target, TestGroupModules.GetValueOrDefault(target, []), TestGroupInternals.GetValueOrDefault(target));
            }),
            // The test support code (crash and file probes) reads PlatformBase internals. Its group targets the runner, so it sees the runner's
            // three areas, the public and Tests areas of the modules the runner's compile environment sees, and the PlatformBase internals.
            new SourceGroup(support, ExecutableName, [], ["PlatformBase"])];

        // POSIX platforms use the shared test support implementation. Private/Common holds the platform-independent support (the
        // interleaving step scheduler state, Docs/Design/Synchronization section 14.3).
        directories.Add(context.Platform == NativePlatform.Windows ? $"{support}/Private/Win64" : $"{support}/Private/Posix");
        directories.Add($"{support}/Private/Common");
        var sources = context.Sources(directories.ToArray()).ToList();
        if (Layer == TestLayer.Engine && Modular)
        {
            var skipped = GlobalStateDirectSources.Select(context.PathInRoot).ToHashSet(StringComparer.OrdinalIgnoreCase);
            sources.RemoveAll(skipped.Contains);
        }

        sources.Add(context.PathInRoot(context.Configuration == NativeConfiguration.Asan
            ? $"{support}/Private/Asan/Enabled.cpp" : $"{support}/Private/Asan/Disabled.cpp"));
        string[] dependencies = Layer switch
        {
            TestLayer.Core => ["Launch", "Task", "Localization", "Serialization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", "Progress", .. DeveloperTestModules],
            TestLayer.Foundation => ["Launch", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", "Progress", .. DeveloperTestModules, .. ObjectDeveloperModules],
            _ => ["Launch", "ObjectSerialization", "ReflectionText", "Config", "LogHost", "TraceControl", "HostConfig", "Localization", "Compression", "AsyncCompression", "Crypto", "TraceCapture", "ModuleLoader", "Progress", .. DeveloperTestModules, .. ObjectDeveloperModules,
                .. EngineTestDependencies, .. (PluginTests ? PluginTestModules.Select(PluginModuleName).ToArray() : Array.Empty<string>())]
        };

        // Object family tests depend on the reflection fixture module (config layer tests on ConfigTestFixtures, curve tests on CurvesTestFixtures). Generated headers (<ReflectionTestFixtures/*.generated.h>)
        // propagate through that module's public includes. Engine layer tests also depend on the World reflection fixture (WorldTestFixtures: save and path
        // test types that derive from World types), the asset fixture (AssetTestFixtures: test asset classes that derive from AssetRuntime's Asset) and the
        // shader parameter fixture (ShaderParameterTestFixtures: generated shader parameter structs, Docs/Design/ShaderSystem section 16.2) and the
        // input actions fixture (InputActionsTestFixtures: reflected handlers for the function binding tests, Docs/Design/InputActions section 5.1) and the
        // curve assets fixture (CurveAssetsTestFixtures: an asset holding the curve sources, Docs/Design/CurveAssets section 3.1).
        string[] fixtures = Layer switch
        {
            TestLayer.Core => [],
            TestLayer.Foundation => ["ReflectionTestFixtures", "ConfigTestFixtures", "CurvesTestFixtures"],
            _ => ["ReflectionTestFixtures", "ConfigTestFixtures", "CurvesTestFixtures", "WorldTestFixtures", "AssetTestFixtures", "ShaderParameterTestFixtures",
                "InputActionsTestFixtures", "CurveAssetsTestFixtures"]
        };
        return new()
        {
            Name = ExecutableName,
            ModuleDirectory = "Programs/FoundationTests",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            Unity = RepositoryUnity.Effective(UnityMode.On, context),
            UnityExclude = UnityStandaloneSources.Select(context.PathInRoot).ToArray(),
            // Core/Source/Runtime (and Engine/Source/Runtime for the engine layer): tests include module Private headers with the
            // <Module/Private/...> spelling. The repository root is not included (so headers of gitignored outputs, user directories and
            // nested working copies do not enter the include inventory).
            PrivateIncludeDirectories = [context.PathInRoot("Core/Source/Runtime"), context.PathInRoot($"{support}/Public"),
                .. (Layer == TestLayer.Engine ? [context.PathInRoot("Engine/Source/Runtime")] : Array.Empty<string>()),
                .. (PluginTests ? PluginTestModules.Select(module => context.PathInRoot(PluginSourceDirectory(module))).ToArray() : Array.Empty<string>())],
            PrivateDefines = ["ROCKS_TESTING=1"],
            // Third-party libraries the engine runner's adapter tests include directly (Docs/Design/BuildTargets section 3.5 BS3a).
            PrivateDependencies = Layer != TestLayer.Engine ? [] :
                Modular ? EngineThirdPartyHeaders.Except(GlobalStateThirdPartyHeaders).ToArray() : EngineThirdPartyHeaders,
            GlobalStateTestConsumer = Layer == TestLayer.Engine && !Modular,
            // ModuleLoader tests load DynamicModuleTestFixture at run time (Docs/Design/ModuleLoader section 3.8): it is linked into this
            // executable and has a module table row, but its headers stay off this module's include path; tests use DynamicModuleTestApi.
            PublicDependencies = [.. dependencies, .. fixtures, "DynamicModuleTestApi"],
            DynamicModules = ["DynamicModuleTestFixture"],
            SourceGroups = groups
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

// Engine layer runner of the Modular engine test target (EngineModular): the EngineTests sources, engine plugin tests included (Docs/Design/BuildTargets
// section 3.8 BS8e, Docs/Design/ModuleLoader section 3.15 ML5a).
public sealed class EngineModularTests : TestsModule
{
    protected override TestLayer Layer => TestLayer.Engine;

    protected override string ExecutableName => "EngineModularTests";

    protected override bool Modular => true;
}
