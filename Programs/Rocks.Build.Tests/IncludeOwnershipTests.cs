using Rocks.Build;

/// <summary>
/// include 소유 모듈 검사(Docs/Design/BuildTargets 3.5절 BD11·BS1b)를 실제 컴파일·의존 기록으로 검사한다. 픽스처(모듈 폴더 = 모듈 이름):
/// Alpha(공개 Alpha/Alpha.h, 비공개 Private/AlphaInternal.h, 시험 도우미 Tests/AlphaTestSupport.h), Beta(Alpha에 공개 의존), Gamma(의존 없음),
/// Delta(Alpha 비공개 폴더를 private include로 받음 — 일부러 보여 주는 허가), Runner(시험 실행 파일, 소스 묶음: Beta/Tests·Gamma/Tests).
/// <list type="bullet">
/// <item>바른 include(공개 의존의 공개 헤더, 허가 폴더, 묶음 대상 환경의 시험 도우미, 추가 모듈, 내부 모듈)는 통과하고 결과에 위반 보고 파일이 없다.</item>
/// <item>위반 하나씩: 의존하지 않은 모듈의 공개 헤더, 의존한 모듈의 비공개 헤더, 묶음 밖 모듈의 시험 도우미, 내부 모듈 없이 비공개 헤더 — 빌드
/// 실패와 문구(소스·헤더·소유 모듈·영역·보는 쪽), 보고 파일.</item>
/// <item>소유 없는 경로(어떤 모듈 폴더에도 없는 헤더)는 보지 않는다. 유니티 묶음은 소스 묶음 경계를 넘지 않는다.</item>
/// <item>공개 헤더 검사 모듈(PublicHeaderCheckOf)의 기록으로 쓰지 않는 공개 의존을 알린다.</item>
/// </list>
/// </summary>
internal static class IncludeOwnershipTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        if (request.Toolchain.Platform == NativePlatform.Android)
        {
            return;
        }

        string fixture = Directory.CreateDirectory(Path.Combine(root, "include-ownership")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        Write("Alpha/Public/Alpha/Alpha.h", "#pragma once\ninline int AlphaValue() noexcept { return 1; }\n");
        Write("Alpha/Private/AlphaInternal.h", "#pragma once\ninline int AlphaInternal() noexcept { return 2; }\n");
        Write("Alpha/Tests/AlphaTestSupport.h", "#pragma once\ninline int AlphaSupport() noexcept { return 3; }\n");
        Write("Gamma/Public/Gamma/Gamma.h", "#pragma once\ninline int GammaValue() noexcept { return 4; }\n");
        Write("Gamma/Tests/GammaTestSupport.h", "#pragma once\ninline int GammaSupport() noexcept { return 5; }\n");
        Write("Shared/Loose.h", "#pragma once\ninline int Loose() noexcept { return 6; }\n");
        string alpha = Write("Alpha/Private/Alpha.cpp", "#include <Alpha/Alpha.h>\n#include \"AlphaInternal.h\"\nint AlphaLink() noexcept { return AlphaValue() + AlphaInternal(); }\n");
        string beta = Write("Beta/Private/Beta.cpp", "#include <Alpha/Alpha.h>\nint BetaLink() noexcept { return AlphaValue(); }\n");
        string gamma = Write("Gamma/Private/Gamma.cpp", "#include <Gamma/Gamma.h>\nint GammaLink() noexcept { return GammaValue(); }\n");
        string delta = Write("Delta/Private/Delta.cpp", "#include <AlphaInternal.h>\nint DeltaLink() noexcept { return AlphaInternal(); }\n");
        // 묶음 Beta/Tests: Beta 환경(Alpha 공개·시험 영역), Loose(소유 없음). 묶음 Gamma/Tests: 추가 모듈 Alpha(공개·시험), 내부 모듈 Alpha(비공개).
        string betaTest = Write("Beta/Tests/BetaTests.cpp",
            "#include <Alpha/Alpha.h>\n#include \"../../Alpha/Tests/AlphaTestSupport.h\"\n#include \"../../Shared/Loose.h\"\nint BetaTest() noexcept { return AlphaValue() + AlphaSupport() + Loose(); }\n");
        string betaTestTwo = Write("Beta/Tests/BetaMoreTests.cpp", "#include <Alpha/Alpha.h>\nint BetaTestTwo() noexcept { return AlphaValue(); }\n");
        string gammaTest = Write("Gamma/Tests/GammaTests.cpp",
            "#include <Gamma/Gamma.h>\n#include \"../../Alpha/Tests/AlphaTestSupport.h\"\n#include \"../../Alpha/Private/AlphaInternal.h\"\nint GammaTest() noexcept { return GammaValue() + AlphaSupport() + AlphaInternal(); }\n");
        string main = Write("Runner/Private/Main.cpp", "int main() { return 0; }\n");
        string Folder(string name) => Path.Combine(fixture, name);
        NativeModuleInput Library(string name, string source, params string[] dependencies) => new()
        {
            Name = name,
            ModuleDirectory = Folder(name),
            Sources = [source],
            PublicIncludeDirectories = Directory.Exists(Path.Combine(Folder(name), "Public")) ? [Path.Combine(Folder(name), "Public")] : [],
            PublicDependencies = dependencies
        };

        IReadOnlyList<NativeModuleInput> Modules(SourceGroup betaGroup, SourceGroup gammaGroup, string? gammaSource = null, string? betaSource = null) =>
        [
            Library("Alpha", alpha),
            Library("Beta", betaSource ?? beta, "Alpha"),
            Library("Gamma", gammaSource ?? gamma),
            Library("Delta", delta, "Alpha") with { PrivateIncludeDirectories = [Path.Combine(Folder("Alpha"), "Private")] },
            new()
            {
                Name = "Runner", Kind = NativeModuleKind.Executable, ModuleDirectory = Folder("Runner"), Sources = [main, betaTest, betaTestTwo, gammaTest],
                PublicDependencies = ["Beta", "Gamma", "Delta"], SourceGroups = [betaGroup, gammaGroup], Unity = UnityMode.On, UnityAnchorFiles = int.MaxValue
            }
        ];
        SourceGroup betaGroup = new(Folder("Beta/Tests"), "Beta", []);
        SourceGroup gammaGroup = new(Folder("Gamma/Tests"), "Gamma", ["Alpha"], ["Alpha"]);
        var builder = new NativeBuildEngine();
        NativeBuildRequest Request(IReadOnlyList<NativeModuleInput> modules, string folder) => request with
        {
            OutputDirectory = Path.Combine(fixture, folder),
            Modules = modules,
            Targets = ["Runner"],
            LinkType = NativeLinkType.Monolithic,
            Configuration = NativeConfiguration.Debug
        };

        NativeBuildResult clean = await builder.BuildAsync(Request(Modules(betaGroup, gammaGroup), "out"));
        Check(clean.Succeeded, "allowed includes must pass the ownership check: " + clean.Failure);
        Check(!File.Exists(Path.Combine(fixture, "out", IncludeOwnership.ReportFileName)), "a clean build leaves no violation report");
        // 유니티 묶음은 소스 묶음 경계를 넘지 않는다: 두 Beta 시험은 한 묶음이고(앵커가 없어 경계가 없다) Gamma 시험·묶음 밖 Main과 섞이지 않는다.
        string unity = Path.Combine(fixture, "out", UnityBuild.DirectoryName, "Runner");
        string[] batches = [.. (Directory.Exists(unity) ? Directory.GetFiles(unity, "*.cpp") : []).Select(File.ReadAllText)];
        Check(batches.Any(text => text.Contains("BetaTests.cpp", StringComparison.Ordinal) && text.Contains("BetaMoreTests.cpp", StringComparison.Ordinal)),
            "the two Beta tests share a unity batch: " + string.Join(" | ", batches));
        Check(batches.All(text => !(text.Contains("Beta", StringComparison.Ordinal) &&
                (text.Contains("GammaTests.cpp", StringComparison.Ordinal) || text.Contains("Main.cpp", StringComparison.Ordinal)))),
            "a unity batch must not mix two source groups: " + string.Join(" | ", batches));
        pass("include ownership: dependency public headers, include grants, test helpers of the target environment, extra and internal modules");

        async Task Rejects(IReadOnlyList<NativeModuleInput> modules, string folder, string expected, string name)
        {
            NativeBuildResult result = await builder.BuildAsync(Request(modules, folder));
            string report = Path.Combine(fixture, folder, IncludeOwnership.ReportFileName);
            Check(!result.Succeeded && result.Failure is not null && result.Failure.Contains("Include ownership check failed", StringComparison.Ordinal) &&
                result.Failure.Contains(expected, StringComparison.Ordinal) && File.Exists(report),
                $"{name}: expected an ownership failure with '{expected}': {result.Failure}");
        }

        string gammaUsesAlpha = Write("Gamma/Private/GammaUsesAlpha.cpp", "#include \"../../Alpha/Public/Alpha/Alpha.h\"\nint GammaLink() noexcept { return AlphaValue(); }\n");
        await Rejects(Modules(betaGroup, gammaGroup, gammaSource: gammaUsesAlpha), "out-public",
            "Alpha/Public/Alpha/Alpha.h, which is in the public area of module Alpha; it is not visible to module Gamma",
            "a public header of a module the source does not depend on");
        string betaUsesInternal = Write("Beta/Private/BetaUsesInternal.cpp", "#include \"../../Alpha/Private/AlphaInternal.h\"\nint BetaLink() noexcept { return AlphaInternal(); }\n");
        await Rejects(Modules(betaGroup, gammaGroup, betaSource: betaUsesInternal), "out-private",
            "which is in the private area of module Alpha; it is not visible to module Beta", "a private header of a dependency");
        await Rejects(Modules(betaGroup, gammaGroup with { Modules = [], Internals = null }), "out-tests",
            "which is in the tests area of module Alpha; it is not visible to the Gamma source group of module Runner", "a test helper outside the group");
        await Rejects(Modules(betaGroup, gammaGroup with { Internals = null }), "out-internal",
            "which is in the private area of module Alpha; it is not visible to the Gamma source group of module Runner", "a private header without an internal module");
        pass("include ownership: public, private and test areas outside the source's visibility fail the build with the source, header and owner");

        // 쓰지 않는 공개 의존: Beta의 공개 헤더 검사 기록에 Alpha 헤더가 없다(Beta에는 공개 헤더가 없고 검사 모듈은 Gamma 헤더 하나를 include한다).
        string check = Write("Checks/BetaCheck.cpp", "#include <Gamma/Gamma.h>\nint BetaCheck() noexcept { return 0; }\n");
        NativeModuleInput[] checkedModules =
        [
            .. Modules(betaGroup, gammaGroup),
            new() { Name = "BetaHeaderCheck", ModuleDirectory = Folder("Checks"), Sources = [check], PublicDependencies = ["Beta", "Gamma"], PublicHeaderCheckOf = "Beta" }
        ];
        NativeBuildResult notice = await builder.BuildAsync(Request(checkedModules, "out-unused") with { Targets = ["Runner", "BetaHeaderCheck"] });
        Check(notice.Succeeded && notice.Notices.Any(line => line.Contains("Unused public dependencies", StringComparison.Ordinal) &&
            line.Contains("Beta: Alpha", StringComparison.Ordinal)), "the public header check record reports unused public dependencies: " +
            notice.Failure + " | " + string.Join(" | ", notice.Notices));
        pass("include ownership: unused public dependencies are reported from the public header check records");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
