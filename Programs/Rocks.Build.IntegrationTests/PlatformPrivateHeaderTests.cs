using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.IntegrationTests;

internal static class PlatformPrivateHeaderTests
{
    internal static async Task RunAsync(NativeToolchain toolchain, string fixture)
    {
        string root = Path.GetFullPath(fixture);
        string output = Path.Combine(root, "out");
        string mac = Path.Combine(root, "Core/Source/Runtime/PlatformBase/Private/Mac");
        string common = Path.Combine(root, "Core/Source/Runtime/PlatformBase/Private/Common");
        string apple = Path.Combine(root, "Core/Source/Runtime/PlatformBase/Private/Apple");
        // Mac rule은 POSIX 공통 backend(Private/Posix)도 함께 나열한다.
        string posix = Path.Combine(root, "Core/Source/Runtime/PlatformBase/Private/Posix");
        string publicPlatform = Path.Combine(root, "Core/Source/Runtime/PlatformBase/Public");
        string publicBase = Path.Combine(root, "Core/Source/Runtime/Base/Public");
        string app = Path.Combine(root, "App");
        foreach (string directory in new[] { mac, common, apple, posix, publicPlatform, publicBase, app })
            Directory.CreateDirectory(directory);

        string header = Path.Combine(apple, "ApplePlatformPath.h");
        string pathSource = Path.Combine(mac, "Path.cpp");
        string appSource = Path.Combine(app, "main.cpp");
        File.WriteAllText(header, "#pragma once\n#define ROCKS_PATH_TEST_VALUE 7\n");
        File.WriteAllText(pathSource,
            "#include \"../Apple/ApplePlatformPath.h\"\nint PathValue() noexcept { return ROCKS_PATH_TEST_VALUE; }\n");
        File.WriteAllText(appSource, "int PathValue() noexcept;\nint main() { return PathValue(); }\n");

        // 실제 Mac rule을 평가하지만 OS API는 사용하지 않는다. 따라서 Windows에서도 의존 추적을
        // 검증할 수 있으며, 이것이 macOS 컴파일이나 실행을 검증했다는 뜻은 아니다.
        var context = new BuildContext(root, output, NativePlatform.MacOS, NativeConfiguration.Debug);
        NativeModuleInput platform = new global::PlatformBase().Configure(context);
        NativeModuleInput baseModule = new global::Base().Configure(context);
        Require(platform.PrivateIncludeDirectories.Contains(Path.GetFullPath(apple), StringComparer.OrdinalIgnoreCase),
            "Mac PlatformBase rule does not declare the Apple private header directory.");
        var application = new NativeModuleInput
        {
            Name = "PathHeaderFixture",
            Kind = NativeModuleKind.Executable,
            Sources = [appSource],
            Dependencies = ["PlatformBase"]
        };
        var request = new NativeBuildRequest
        {
            RootDirectory = root,
            OutputDirectory = output,
            Toolchain = toolchain,
            Modules = [baseModule, platform, application],
            Targets = [application.Name],
            MaxParallelism = 2
        };
        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Build()
        {
            var result = await builder.BuildAsync(request);
            Require(result.Succeeded, "Build failed: " + result.Failure + "\n" +
                string.Join("\n", result.Actions.Select(action => action.StdOut + action.StdErr)));
            return result;
        }

        var initial = await Build();
        Require(initial.Actions.Any(action => action.Module == "PlatformBase" &&
            action.Kind == "Compile" && action.Executed), "Initial Mac-shaped path source did not compile.");
        var initialRun = await ProcessRunner.RunAsync(new(initial.ModuleOutputs[application.Name], [], root,
            toolchain.Environment, Timeout: TimeSpan.FromSeconds(30)));
        Require(initialRun.ExitCode == 7, "Initial executable did not use the Apple header.");
        var warm = await Build();
        Require(warm.ExecutedActions == 0, "Unchanged build was not a no-op.");

        File.WriteAllText(header, "#pragma once\n#define ROCKS_PATH_TEST_VALUE 11\n");
        var changed = await Build();
        Require(changed.Actions.Any(action => action.Module == "PlatformBase" &&
            action.Kind == "Compile" && action.Executed), "Changing Apple header did not recompile PlatformBase.");
        Require(changed.Actions.Any(action => action.Module == application.Name &&
            action.Kind == "Link" && action.Executed), "Changing Apple header did not relink the executable.");
        var changedRun = await ProcessRunner.RunAsync(new(changed.ModuleOutputs[application.Name], [], root,
            toolchain.Environment, Timeout: TimeSpan.FromSeconds(30)));
        Require(changedRun.ExitCode == 11, "Rebuilt executable retained the stale Apple header value.");
        var final = await Build();
        Require(final.ExecutedActions == 0, "Build did not settle after the header change.");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
