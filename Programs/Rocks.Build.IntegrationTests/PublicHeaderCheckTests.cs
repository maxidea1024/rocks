using Rocks.Build;
using Rocks.Build.Cli;

namespace Rocks.Build.IntegrationTests;

// 모듈별 공개 헤더 검사(Docs/Design/BuildTargets.ko.md 3.5절 BD9·BS1a)의 실제 컴파일 검사다. 픽스처: Alpha(공개 헤더 Alpha.h), Beta(Alpha에
// 의존하고 공개 헤더가 Alpha.h를 include), Gamma(의존 없이 공개 헤더가 Alpha.h를 include), 실행 파일 App(Beta·Gamma에 의존). 예전 검사 모듈
// 하나(도달한 모든 라이브러리를 공개 의존으로 둠)는 Gamma의 빠진 의존을 Alpha로 메웠다.
internal static class PublicHeaderCheckTests
{
    internal static async Task RunAsync(NativeToolchain toolchain, string fixture)
    {
        string root = Directory.CreateDirectory(Path.GetFullPath(fixture)).FullName;
        string Write(string relative, string content)
        {
            string path = Path.Combine(root, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        Write("alpha/include/Alpha/Alpha.h", "#pragma once\nint AlphaValue() noexcept;\n");
        Write("beta/include/Beta/Beta.h", "#pragma once\n#include <Alpha/Alpha.h>\ninline int BetaValue() noexcept { return AlphaValue() + 1; }\n");
        Write("gamma/include/Gamma/Gamma.h", "#pragma once\n#include <Alpha/Alpha.h>\ninline int GammaValue() noexcept { return AlphaValue() + 2; }\n");
        string alphaSource = Write("alpha/alpha.cpp", "#include <Alpha/Alpha.h>\nint AlphaValue() noexcept { return 1; }\n");
        string betaSource = Write("beta/beta.cpp", "#include <Beta/Beta.h>\nint BetaTwice() noexcept { return BetaValue() * 2; }\n");
        string gammaSource = Write("gamma/gamma.cpp", "#include <Alpha/Alpha.h>\n#include <Gamma/Gamma.h>\nint GammaTwice() noexcept { return GammaValue() * 2; }\n");
        string appSource = Write("app/main.cpp", "#include <Beta/Beta.h>\n#include <Gamma/Gamma.h>\nint main() { return BetaValue() + GammaValue() == 5 ? 0 : 1; }\n");
        NativeModuleInput Library(string name, string source, params string[] dependencies) => new()
        {
            Name = name,
            Sources = [source],
            PublicIncludeDirectories = [Path.Combine(root, name.ToLowerInvariant(), "include")],
            PublicDependencies = dependencies
        };

        // Gamma는 Alpha에 의존하지 않는다(소스는 App의 include 경로로 Alpha를 연다 — 공개 헤더 검사만 잡는다).
        List<NativeModuleInput> Modules(params string[] gammaDependencies) =>
        [
            Library("Alpha", alphaSource),
            Library("Beta", betaSource, "Alpha"),
            Library("Gamma", gammaSource, gammaDependencies) with
            {
                PrivateIncludeDirectories = [Path.Combine(root, "alpha", "include")]
            },
            new() { Name = "App", Kind = NativeModuleKind.Executable, Sources = [appSource], PublicDependencies = ["Beta", "Gamma", "Alpha"] }
        ];

        string output = Path.Combine(root, "out");
        List<NativeModuleInput> missing = Modules();
        List<string> targets = ["App"];
        IReadOnlyList<string> checks = PublicHeaderChecks.Add(missing, targets, output, toolchain);
        Require(checks.SequenceEqual(["PublicHeaderChecks_Alpha", "PublicHeaderChecks_Beta", "PublicHeaderChecks_Gamma"]),
            "one check module per library module with public headers: " + string.Join(", ", checks));
        NativeModuleInput gammaCheck = missing.Single(module => module.Name == "PublicHeaderChecks_Gamma");
        Require(gammaCheck.PublicDependencies.SequenceEqual(["Gamma"]) && gammaCheck.Sources.Count == 1 &&
            gammaCheck.Unity == UnityMode.Off && gammaCheck.PchUsage == PchUsage.Off,
            "a check module depends only on its module and compiles each header alone without unity or PCH");

        var builder = new NativeBuildEngine();
        NativeBuildRequest Request(List<NativeModuleInput> modules, IReadOnlyList<string> checkNames, string folder) => new()
        {
            RootDirectory = root,
            OutputDirectory = Path.Combine(root, folder),
            Toolchain = toolchain,
            Modules = modules,
            Targets = ["App", .. checkNames],
            MaxParallelism = 2
        };
        NativeBuildResult failed = await builder.BuildAsync(Request(missing, checks, "out"));
        Require(!failed.Succeeded && failed.Actions.Any(action => action.Module == "PublicHeaderChecks_Gamma" && action.Kind == "Compile" && !action.Succeeded) &&
            failed.Actions.Where(action => action.Module is "PublicHeaderChecks_Alpha" or "PublicHeaderChecks_Beta").All(action => action.Succeeded),
            "a public header that includes a module it does not depend on must fail only its own check: " + failed.Failure);

        List<NativeModuleInput> fixedModules = Modules("Alpha");
        IReadOnlyList<string> fixedChecks = PublicHeaderChecks.Add(fixedModules, targets, Path.Combine(root, "out-fixed"), toolchain);
        NativeBuildResult passed = await builder.BuildAsync(Request(fixedModules, fixedChecks, "out-fixed"));
        Require(passed.Succeeded, "public header checks pass once the dependency is declared: " + passed.Failure + "\n" +
            string.Join("\n", passed.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr)));
        Console.WriteLine("PASS public header checks: one check module per module, compiled with its public environment only");
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
