using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 엔진 모듈 테스트를 한 실행 파일로 묶는 규칙이다. 각 모듈의 Tests/ 폴더와 테스트 runner(이 폴더의
/// Private/), 테스트 지원 코드(Core/Source/Developer/TestSupport)를 함께 빌드한다.
/// 새 모듈의 테스트를 추가하려면 해당 계층 목록(RuntimeTestModules·ObjectTestModules·EngineTestModules)에
/// 모듈 이름을 넣는다.
/// </summary>
public abstract class TestsModule : ModuleRules
{
    /// <summary>테스트 실행 파일이 포함하는 계층이다. 위 계층은 아래 계층의 테스트를 모두 포함한다.</summary>
    protected enum TestLayer { Core, Foundation, Engine }

    // Object 계열이 아닌 런타임 모듈의 테스트다.
    private static readonly string[] RuntimeTestModules = ["Base", "Core", "PlatformBase", "Task", "Localization", "Serialization"];

    // Object 계열 모듈의 테스트다(FoundationTests·EngineTests에 들어간다).
    private static readonly string[] ObjectTestModules = ["ObjectRuntime", "ObjectSerialization"];

    // 엔진 계층 모듈의 테스트다(EngineTests에만 들어간다). 경로는 Engine/Source 아래 Group/Module이다.
    private static readonly string[] EngineTestModules = ["Runtime/Input", "Runtime/PlatformWindow", "Runtime/World", "Runtime/Engine", "Runtime/AssetRuntime", "Developer/AssetTools"];

    // EngineTests가 링크하는 엔진 모듈이다(테스트가 쓰는 최상위 모듈만 둔다. 하위 의존은 전파된다).
    private static readonly string[] EngineTestDependencies = ["PlatformWindow", "Engine"];

    protected abstract TestLayer Layer { get; }

    protected abstract string ExecutableName { get; }

    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        bool objects = Layer != TestLayer.Core;
        var directories = new List<string> { "Programs/FoundationTests/Private" };
        directories.AddRange(RuntimeTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));
        if (objects)
            directories.AddRange(ObjectTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));
        if (Layer == TestLayer.Engine)
            directories.AddRange(EngineTestModules.Select(module => $"Engine/Source/{module}/Tests")
                .Where(directory => Directory.Exists(context.PathInRoot(directory))));

        // POSIX 플랫폼은 공통 테스트 지원 구현을 쓴다.
        directories.Add(context.Platform == NativePlatform.Windows ? $"{support}/Private/Win64" : $"{support}/Private/Posix");
        var sources = context.Sources(directories.ToArray()).ToList();
        sources.Add(context.PathInRoot(context.Configuration == NativeConfiguration.Asan
            ? $"{support}/Private/Asan/Enabled.cpp" : $"{support}/Private/Asan/Disabled.cpp"));
        string[] dependencies = Layer switch
        {
            TestLayer.Core => ["Task", "Localization", "Serialization"],
            TestLayer.Foundation => ["ObjectSerialization", "Localization"],
            _ => ["ObjectSerialization", "Localization", .. EngineTestDependencies]
        };
        return new()
        {
            Name = ExecutableName,
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            // 저장소 루트: 메타 생성 코드가 fixture를 루트 기준 경로로 include한다.
            PrivateIncludeDirectories = [context.RootDirectory, context.PathInRoot($"{support}/Public"),
                Path.Combine(context.OutputDirectory, "Generated")],
            PrivateDefines = ["ROCKS_TESTING=1"],
            Dependencies = dependencies
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
