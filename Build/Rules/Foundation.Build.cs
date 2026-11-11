using Rocks.Build;
using Rocks.Build.Rules;

public sealed class Base : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "Base",
        PublicIncludeDirectories = [context.PathInRoot("Engine/Base/Public")]
    };
}

public sealed class PlatformBase : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "PlatformBase",
        // POSIX 플랫폼은 공통 Posix 구현과 OS hook(Apple·Android)을 함께 빌드한다.
        Sources = context.Platform switch
        {
            NativePlatform.MacOS => context.Sources("Engine/PlatformBase/Private/Mac", "Engine/PlatformBase/Private/Apple",
                "Engine/PlatformBase/Private/Posix", "Engine/PlatformBase/Private/Common"),
            NativePlatform.Android => context.Sources("Engine/PlatformBase/Private/Android",
                "Engine/PlatformBase/Private/Posix", "Engine/PlatformBase/Private/Common"),
            _ => context.Sources($"Engine/PlatformBase/Private/{context.PlatformDirectory}", "Engine/PlatformBase/Private/Common")
        },
        PublicIncludeDirectories = [context.PathInRoot("Engine/PlatformBase/Public")],
        // Mac 소스가 sibling Apple 공통 헤더를 사용하므로 증분 빌드의 입력에도 포함한다.
        PrivateIncludeDirectories = context.Platform == NativePlatform.MacOS
            ? [context.PathInRoot("Engine/PlatformBase/Private/Apple")]
            : [],
        Dependencies = ["Base"]
    };
}

public abstract class RuntimeModule : ModuleRules
{
    protected abstract string ModuleName { get; }
    protected abstract string[] RequiredModules { get; }

    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = ModuleName,
        Sources = context.Sources($"Engine/{ModuleName}/Private"),
        PublicIncludeDirectories = [context.PathInRoot($"Engine/{ModuleName}/Public")],
        Dependencies = RequiredModules
    };
}

public sealed class Core : RuntimeModule
{
    protected override string ModuleName => "Core";
    protected override string[] RequiredModules => ["PlatformBase"];
}

public sealed class TaskRuntime : RuntimeModule
{
    protected override string ModuleName => "Task";
    protected override string[] RequiredModules => ["Core"];
}

public sealed class Localization : RuntimeModule
{
    protected override string ModuleName => "Localization";
    protected override string[] RequiredModules => ["Core"];
}

public sealed class Serialization : RuntimeModule
{
    protected override string ModuleName => "Serialization";
    protected override string[] RequiredModules => ["Core"];
}

public sealed class MetaSchema : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "MetaSchema",
        PublicIncludeDirectories = [context.PathInRoot("Engine/MetaSchema/Public")],
        Dependencies = ["Core"]
    };
}

public sealed class ObjectRuntime : RuntimeModule
{
    protected override string ModuleName => "ObjectRuntime";
    protected override string[] RequiredModules => ["MetaSchema", "Task"];
}

public sealed class ObjectSerialization : RuntimeModule
{
    protected override string ModuleName => "ObjectSerialization";
    protected override string[] RequiredModules => ["ObjectRuntime", "Serialization"];
}

public abstract class TestsModule : ModuleRules
{
    protected abstract bool Objects { get; }

    public override NativeModuleInput Configure(BuildContext context)
    {
        var directories = new List<string> { "Tests/Foundation", "Tests/Core", "Tests/Platform",
            "Tests/Localization", "Tests/Serialization",
            // POSIX 플랫폼은 공통 테스트 지원 구현을 쓴다.
            context.Platform == NativePlatform.Windows ? "Tests/Support/Private/Win64" : "Tests/Support/Private/Posix" };
        if (Objects)
            directories.AddRange(["Tests/Object", "Tests/Reflection", "Tests/ObjectSerialization"]);
        var sources = context.Sources(directories.ToArray()).ToList();
        sources.Add(context.PathInRoot(context.Configuration == NativeConfiguration.Asan
            ? "Tests/Support/Private/Asan/Enabled.cpp" : "Tests/Support/Private/Asan/Disabled.cpp"));
        return new()
        {
            Name = Objects ? "FoundationTests" : "CoreFoundationTests",
            Kind = NativeModuleKind.Executable,
            Sources = sources,
            PrivateIncludeDirectories = [context.RootDirectory, context.PathInRoot("Tests/Support"),
                Path.Combine(context.OutputDirectory, "Generated")],
            PrivateDefines = ["ROCKS_TESTING=1"],
            Dependencies = Objects ? ["ObjectSerialization", "Localization"] : ["Task", "Localization", "Serialization"]
        };
    }
}

public sealed class FoundationTests : TestsModule { protected override bool Objects => true; }
public sealed class CoreFoundationTests : TestsModule { protected override bool Objects => false; }
