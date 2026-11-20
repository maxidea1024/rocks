using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 링크 방식(Docs/Design/BuildTargets.ko.md 3.8절, BS8a)을 실제 컴파일·링크로 검사한다. 픽스처: Lib ← Mid ← App(실행 파일). Lib·Mid는 모듈
/// API 매크로(<c>ROCKS_LIB_API</c>·<c>ROCKS_MID_API</c>)로 함수를 export한다.
/// <list type="bullet">
/// <item>Modular: Lib·Mid가 공유 라이브러리(<c>&lt;타깃&gt;-&lt;모듈&gt;-&lt;플랫폼&gt;-&lt;구성&gt;</c>)가 되고 App이 import해 실행된다. 각 compile은
/// <c>ROCKS_MONOLITHIC=0</c>을 받고 정의 헤더의 API 매크로는 자기 모듈 export, 의존 모듈 import다.</item>
/// <item>export가 빠진 함수는 Modular 링크에서 미해결 기호다(Monolithic은 그대로 링크된다).</item>
/// <item>Monolithic: 같은 픽스처가 정적 링크로 실행되고 API 매크로는 빈 값, <c>ROCKS_MONOLITHIC=1</c>이다.</item>
/// <item>입력 오류: Modular Release 구성, 규칙이 정의한 <c>ROCKS_MONOLITHIC</c>·<c>ROCKS_*_API</c>.</item>
/// </list>
/// </summary>
internal static class ModularLinkTests
{
    private const string LibHeader = "#pragma once\nROCKS_LIB_API int LibValue() noexcept;\n";

    private const string MidHeader = "#pragma once\nROCKS_MID_API int MidValue() noexcept;\n";

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        if (request.Toolchain.Platform == NativePlatform.Android)
        {
            return;
        }

        string fixture = Directory.CreateDirectory(Path.Combine(root, "modular-link")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        string Include(string module) => Path.GetDirectoryName(Path.GetDirectoryName(Write(module + "/include/" + module + "/" + module + ".h",
            module == "lib" ? LibHeader : MidHeader)))!;
        string libInclude = Include("lib");
        string midInclude = Include("mid");
        string libSource = Write("lib/lib.cpp", "#include <lib/lib.h>\nint LibValue() noexcept { return 7; }\n");
        string midSource = Write("mid/mid.cpp", "#include <mid/mid.h>\n#include <lib/lib.h>\nint MidValue() noexcept { return LibValue() + 1; }\n");
        string mainSource = Write("app/main.cpp", "#include <mid/mid.h>\nint main() { return MidValue() == 8 ? 0 : 1; }\n");
        NativeModuleInput[] modules =
        [
            new() { Name = "Lib", Sources = [libSource], PublicIncludeDirectories = [libInclude] },
            new() { Name = "Mid", Sources = [midSource], PublicIncludeDirectories = [midInclude], PublicDependencies = ["Lib"] },
            new() { Name = "App", Kind = NativeModuleKind.Executable, Sources = [mainSource], PublicDependencies = ["Mid"] }
        ];
        NativeBuildRequest modular = request with
        {
            OutputDirectory = Path.Combine(fixture, "out-modular"),
            Modules = modules,
            Targets = ["App"],
            TargetName = "Fixture",
            Configuration = NativeConfiguration.Debug,
            LinkType = NativeLinkType.Modular
        };
        var builder = new NativeBuildEngine();
        NativeBuildResult built = await builder.BuildAsync(modular);
        Check(built.Succeeded, "Modular fixture build failed: " + Failure(built));
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        string Shared(string module) => Path.Combine(modular.OutputDirectory, "bin",
            LinkTypes.SharedLibraryStem("Fixture", module, request.Toolchain.Platform, NativeConfiguration.Debug) +
            (windows ? ".dll" : request.Toolchain.Platform == NativePlatform.MacOS ? ".dylib" : ".so"));
        Check(File.Exists(Shared("Lib")) && File.Exists(Shared("Mid")), "Modular build must produce one shared library per module");
        Check(Path.GetFileName(Shared("Lib")).Contains("Fixture-Lib-" + request.Toolchain.Platform + "-Debug", StringComparison.Ordinal),
            "shared library name: " + Shared("Lib"));
        ProcessResult run = await ProcessRunner.RunAsync(new(built.ModuleOutputs["App"], [], fixture, request.Toolchain.Environment));
        Check(run.ExitCode == 0, "Modular executable must call the exported functions: exit " + run.ExitCode + " " + run.StdErr);
        string midDefinitions = File.ReadAllText(ModuleDefinitions.PathOf(modular.OutputDirectory, "Mid"));
        Check(midDefinitions.Contains("#define ROCKS_MID_API " + LinkTypes.ExportSpelling(request.Toolchain.Platform), StringComparison.Ordinal) &&
            midDefinitions.Contains("#define ROCKS_LIB_API " + LinkTypes.ImportSpelling(request.Toolchain.Platform), StringComparison.Ordinal),
            "Modular API macros must export the own module and import the others:\n" + midDefinitions);
        NativePlan plan = ActionGraph.Create(modular with { RootDirectory = Path.GetFullPath(modular.RootDirectory) });
        Check(plan.Actions.Where(action => action.Kind == "Compile").All(action => string.Join(" ", action.Arguments)
            .Contains(LinkTypes.MonolithicDefine(NativeLinkType.Modular), StringComparison.Ordinal)), "every Modular compile must define ROCKS_MONOLITHIC=0");
        pass("modular link: one shared library per module, exported calls across modules run");

        // export가 빠진 함수는 공유 라이브러리 경계를 넘지 못한다(링크 오류). 같은 소스가 Monolithic에서는 링크된다.
        string hiddenHeader = Write("lib-hidden/include/lib/lib.h", "#pragma once\nint LibValue() noexcept;\n");
        NativeModuleInput[] hidden = modules.Select(module => module.Name == "Lib"
            ? module with { PublicIncludeDirectories = [Path.GetDirectoryName(Path.GetDirectoryName(hiddenHeader))!] } : module).ToArray();
        NativeBuildResult unexported = await builder.BuildAsync(modular with { OutputDirectory = Path.Combine(fixture, "out-hidden"), Modules = hidden });
        Check(!unexported.Succeeded && unexported.Actions.Any(action => action.Kind == "Link" && !action.Succeeded),
            "a function without the module API macro must not link across shared libraries");
        pass("modular link: a function without the module API macro is not exported");

        NativeBuildRequest monolithic = modular with { OutputDirectory = Path.Combine(fixture, "out-monolithic"), LinkType = NativeLinkType.Monolithic };
        NativeBuildResult statically = await builder.BuildAsync(monolithic);
        Check(statically.Succeeded, "Monolithic fixture build failed: " + Failure(statically));
        ProcessResult staticRun = await ProcessRunner.RunAsync(new(statically.ModuleOutputs["App"], [], fixture, request.Toolchain.Environment));
        Check(staticRun.ExitCode == 0 && !Directory.EnumerateFiles(Path.Combine(monolithic.OutputDirectory, "bin"), "*Fixture-*").Any(),
            "Monolithic fixture must link statically");
        Check(File.ReadAllText(ModuleDefinitions.PathOf(monolithic.OutputDirectory, "Mid")).Contains("#define ROCKS_MID_API \n", StringComparison.Ordinal),
            "Monolithic API macros must be empty");
        pass("modular link: the same sources link monolithically with empty API macros");

        Rejects(modular with { Configuration = NativeConfiguration.Release }, "supports only the Debug and Development configurations", "Modular Release");
        Rejects(modular with { Modules = modules.Select(module => module.Name == "Lib" ? module with { PublicDefines = ["ROCKS_LIB_API="] } : module).ToArray() },
            "is reserved", "a rule-defined module API macro");
        Rejects(modular with { Modules = modules.Select(module => module.Name == "Lib" ? module with { PrivateDefines = ["ROCKS_MONOLITHIC=1"] } : module).ToArray() },
            "is reserved", "a rule-defined ROCKS_MONOLITHIC");
        pass("modular link: Release Modular and reserved link macros are rejected");
    }

    private static void Rejects(NativeBuildRequest request, string expected, string name)
    {
        try
        {
            InputValidation.Validate(request);
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains(expected, StringComparison.Ordinal), $"{name}: unexpected message: {error.Message}");
            return;
        }

        Check(false, name + " was not rejected");
    }

    private static string Failure(NativeBuildResult result) =>
        result.Failure + "\n" + string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr));

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
