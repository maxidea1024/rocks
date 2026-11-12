using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 엔진 모듈 테스트를 한 실행 파일로 묶는 규칙이다. 각 모듈의 Tests/ 폴더와 테스트 runner(이 폴더의
/// Private/), 테스트 지원 코드(Core/Source/Developer/TestSupport)를 함께 빌드한다.
/// 새 모듈의 테스트를 추가하려면 RuntimeTestModules(또는 ObjectTestModules)에 모듈 이름을 넣는다.
/// </summary>
public abstract class TestsModule : ModuleRules
{
    // Object 계열이 아닌 런타임 모듈의 테스트다.
    private static readonly string[] RuntimeTestModules = ["Base", "Core", "PlatformBase", "Task", "Localization", "Serialization"];

    // Object 계열 모듈의 테스트다(FoundationTests에만 들어간다).
    private static readonly string[] ObjectTestModules = ["ObjectRuntime", "ObjectSerialization"];

    protected abstract bool Objects { get; }

    public override NativeModuleInput Configure(BuildContext context)
    {
        const string support = "Core/Source/Developer/TestSupport";
        var directories = new List<string> { "Programs/FoundationTests/Private" };
        directories.AddRange(RuntimeTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));
        if (Objects)
            directories.AddRange(ObjectTestModules.Select(module => $"Core/Source/Runtime/{module}/Tests"));

        // POSIX 플랫폼은 공통 테스트 지원 구현을 쓴다.
        directories.Add(context.Platform == NativePlatform.Windows ? $"{support}/Private/Win64" : $"{support}/Private/Posix");
        var sources = context.Sources(directories.ToArray()).ToList();
        sources.Add(context.PathInRoot(context.Configuration == NativeConfiguration.Asan
            ? $"{support}/Private/Asan/Enabled.cpp" : $"{support}/Private/Asan/Disabled.cpp"));
        return new()
        {
            Name = Objects ? "FoundationTests" : "CoreFoundationTests",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            // 저장소 루트: 메타 생성 코드가 fixture를 루트 기준 경로로 include한다.
            PrivateIncludeDirectories = [context.RootDirectory, context.PathInRoot($"{support}/Public"),
                Path.Combine(context.OutputDirectory, "Generated")],
            PrivateDefines = ["ROCKS_TESTING=1"],
            Dependencies = Objects ? ["ObjectSerialization", "Localization"] : ["Task", "Localization", "Serialization"]
        };
    }
}

public sealed class FoundationTests : TestsModule { protected override bool Objects => true; }
public sealed class CoreFoundationTests : TestsModule { protected override bool Objects => false; }
