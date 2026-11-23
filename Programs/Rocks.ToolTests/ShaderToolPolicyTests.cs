using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// 셰이더 도구 정책(Docs/Design/ShaderSystem.ko.md 9절)의 저장소 실행과 음성 대조다: 도구 표 참조(shader-tool-table), binary 로드
// (shader-tool-binary-load), 워커 격리(shader-worker-isolation, 11.1절 SC1), Release 배제(release-*).
internal static class ShaderToolPolicyTests
{
    private static readonly string[] BinaryLibraries = ["DirectXShaderCompiler"];

    private static readonly string[] BinaryHeaders = ["dxc/dxcapi.h"];

    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryShaderTools", () => NoFailures(PolicyChecks.ShaderToolReferences(options.Root)));
        suite.Add("Policy.ShaderToolReferenceControls", ReferenceControls);
        suite.Add("Policy.RepositoryReleaseExclusion", () =>
        {
            ReleaseExclusionResult result = ReleaseExclusion.Check(options.Root, ReleaseExclusion.DefaultRulesAssembly(options.Root));
            NoFailures(result.Failures);
            // 알려진 예외 두 건(D3D12Graphics d3dcompiler, Windows OpenGLGraphics)은 SH2까지 실제로 걸리고 알림으로 나온다.
            Verify.That(result.Notices.Any(notice => notice.Contains("d3dcompiler", StringComparison.Ordinal)) &&
                result.Notices.Any(notice => notice.Contains("release-windows-opengl", StringComparison.Ordinal)),
                "known Release exceptions were not reported: " + string.Join('\n', result.Notices));
        });
        suite.Add("Policy.ReleaseExclusionControls", ReleaseControls);
    }

    private static void ReferenceControls()
    {
        using TemporaryDirectory temporary = new();
        IReadOnlyList<CheckFailure> Run() => PolicyChecks.ShaderToolReferences(temporary.Root, BinaryLibraries, BinaryHeaders);

        // 양성 대조: 메인 프로세스는 표·너겟을, 공용 모듈은 표를, 워커 본체와 형식 모듈은 공용 모듈·표·로더·binary 헤더·절대 경로 열기를,
        // Runtime은 OpenSystem을 쓴다.
        temporary.Write("Engine/Source/Developer/ShaderCompiler/ShaderCompiler.Build.cs",
            "PublicDependencies => [\"ShaderCompilerCommon\", \"AssetNugget\", \"ShaderToolTable\"];\n");
        temporary.Write("Engine/Source/Developer/ShaderCompiler/Private/Keys.cpp",
            "#include <ShaderToolTable/ShaderToolTable.h>\n#include <AssetNugget/NuggetKey.h>\n");
        temporary.Write("Engine/Source/Developer/ShaderCompilerCommon/ShaderCompilerCommon.Build.cs", "PublicDependencies => [\"ShaderToolTable\"];\n");
        temporary.Write("Engine/Source/Developer/ShaderCompilerCommon/Private/Format.cpp", "#include <ShaderToolTable/ShaderToolTable.h>\n");
        temporary.Write("Engine/Source/Developer/ShaderWorker/ShaderWorker.Build.cs",
            "PublicDependencies => [\"ShaderCompilerCommon\", \"ShaderToolLoader\", \"ShaderToolTable\"];\n");
        temporary.Write("Engine/Source/Developer/ShaderWorker/Private/Worker.cpp",
            "#include <ShaderCompilerCommon/ShaderFormatDefinition.h>\n#include <ShaderToolLoader/ShaderToolLoader.h>\n" +
            "#include <ShaderToolTable/ShaderToolTable.h>\n// #include <ShaderCompiler/ShaderJobKey.h> in a comment is not code\n");
        temporary.Write("Programs/ShaderCompileWorker/ShaderCompileWorker.Build.cs", "PublicDependencies = [\"ShaderWorker\"];\n");
        temporary.Write("Engine/Source/Developer/ShaderFormatD3D/ShaderFormatD3D.Build.cs",
            "PublicDependencies => [\"ShaderToolLoader\", \"ShaderToolTable\"]; Headers(\"DirectXShaderCompiler\");\n");
        temporary.Write("Engine/Source/Developer/ShaderFormatD3D/Private/Win64/Dxc.cpp",
            "#include <ShaderToolLoader/ShaderToolLoader.h>\n#include <dxc/dxcapi.h>\nvoid F(platform::DynamicLibrary& l) { (void)l.Open(\"C:/x.dll\"); }\n");
        temporary.Write("Engine/Source/Runtime/Probe/Private/Vulkan.cpp",
            "#include <Platform/DynamicLibrary.h>\nvoid G(platform::DynamicLibrary& l) { (void)l.OpenSystem(\"vulkan-1.dll\"); }\n" +
            "// #include <ShaderToolTable/ShaderToolTable.h> and l.Open(path) in a comment are not code\n");
        temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// depends on \"ShaderToolTable\" only in a comment\nPublicDependencies => [\"Core\"];\n");
        // SH0 도구 스파이크는 워커가 생기기 전에 도구를 직접 여는 측정 프로그램이다.
        temporary.Write("Programs/ShaderToolSpike/ShaderToolSpike.Build.cs",
            "PublicDependencies = [\"ShaderToolLoader\", \"ShaderToolTable\"]; Headers(\"DirectXShaderCompiler\");\n");
        temporary.Write("Programs/ShaderToolSpike/Private/Win64/Dxc.cpp",
            "#include <ShaderToolLoader/ShaderToolLoader.h>\n#include <dxc/dxcapi.h>\n");
        NoFailures(Run());

        (string File, string Content, string Rule)[] cases =
        [
            ("Engine/Source/Runtime/Probe/Private/Table.cpp", "#include <ShaderToolTable/ShaderToolTable.h>\n", "shader-tool-table"),
            ("Engine/Source/Runtime/Probe/Probe.Build.cs", "PublicDependencies => [\"ShaderToolTable\"];\n", "shader-tool-table"),
            ("Engine/Source/Runtime/Probe/Probe.Build.cs", "Headers(\"DirectXShaderCompiler\");\n", "shader-tool-binary-load"),
            ("Engine/Source/Runtime/Probe/Private/Dxc.cpp", "#include <dxc/dxcapi.h>\n", "shader-tool-binary-load"),
            ("Engine/Source/Runtime/Probe/Private/Open.cpp",
                "#include <Platform/DynamicLibrary.h>\nvoid H(platform::DynamicLibrary& l, const char* p) { (void)l.Open(p); }\n", "shader-tool-binary-load"),
            // 메인 프로세스(ShaderCompiler)는 표는 쓰지만 binary를 열지 않는다.
            ("Engine/Source/Developer/ShaderCompiler/Private/Load.cpp", "#include <ShaderToolLoader/ShaderToolLoader.h>\n", "shader-tool-binary-load"),
            ("Engine/Source/Developer/ShaderCompiler/ShaderCompiler.Build.cs", "PublicDependencies => [\"ShaderToolTable\", \"ShaderToolLoader\"];\n",
                "shader-tool-binary-load"),
            // 이름이 비슷한 폴더는 형식 모듈이 아니다.
            ("Engine/Source/Developer/ShaderFormat/Private/Load.cpp", "#include <ShaderToolLoader/ShaderToolLoader.h>\n", "shader-tool-binary-load"),
            // 스파이크와 이름만 비슷한 프로그램 폴더는 허용 위치가 아니다.
            ("Programs/ShaderToolSpikeOld/Private/Dxc.cpp", "#include <dxc/dxcapi.h>\n", "shader-tool-binary-load"),
            // 워커 쪽은 메인 프로세스 전용 모듈(ShaderCompiler)과 에셋 너겟에 닿지 않는다(shader-worker-isolation).
            ("Engine/Source/Developer/ShaderWorker/ShaderWorker.Build.cs", "PublicDependencies => [\"ShaderCompilerCommon\", \"ShaderCompiler\"];\n",
                "shader-worker-isolation"),
            ("Engine/Source/Developer/ShaderWorker/Private/Nugget.cpp", "#include <AssetNugget/NuggetCache.h>\n", "shader-worker-isolation"),
            ("Engine/Source/Developer/ShaderFormatD3D/Private/Key.cpp", "#include <ShaderCompiler/ShaderJobKey.h>\n", "shader-worker-isolation"),
            ("Engine/Source/Developer/ShaderFormatVulkan/ShaderFormatVulkan.Build.cs", "PublicDependencies => [\"AssetNugget\"];\n",
                "shader-worker-isolation"),
            ("Programs/ShaderCompileWorker/ShaderCompileWorker.Build.cs", "PublicDependencies = [\"ShaderWorker\", \"AssetNugget\"];\n",
                "shader-worker-isolation"),
            ("Programs/ShaderCompileWorker/Private/Main.cpp", "#include \"ShaderCompiler/ShaderPreprocessor.h\"\n", "shader-worker-isolation")
        ];
        foreach ((string file, string content, string rule) in cases)
        {
            string? original = File.Exists(temporary.FilePath(file)) ? File.ReadAllText(temporary.FilePath(file)) : null;
            temporary.Write(file, content);
            IReadOnlyList<CheckFailure> failures = Run();
            Verify.That(failures.Count == 1 && failures[0].Rule == rule && failures[0].Path == file,
                $"{file} ({rule}) escaped or over-reported: {string.Join('\n', failures)}");
            if (original is null)
            {
                File.Delete(temporary.FilePath(file));
            }
            else
            {
                temporary.Write(file, original);
            }
        }

        NoFailures(Run());
    }

    private static void ReleaseControls()
    {
        Verify.That(new DefaultTarget().Type == TargetType.Game, "a target without a type must be a Game target (UE TargetRules.Type default)");
        NativeModuleInput[] modules =
        [
            new() { Name = "Game", Kind = NativeModuleKind.Executable, PublicDependencies = ["Engine", "Renderer"] },
            new() { Name = "Engine", PublicDependencies = ["Core"] },
            new() { Name = "Core" },
            new() { Name = "Renderer", PublicDependencies = ["Engine"] },
            new() { Name = "Cooker", PublicDependencies = ["Engine"] },
            new() { Name = "D3D12Graphics", SystemLibraries = ["d3d12", "d3dcompiler"] },
            new() { Name = "Dxc", PrebuiltLibraries = ["lib/dxcompiler.lib"] },
            new() { Name = "OpenGLGraphics", SystemLibraries = ["opengl32"] }
        ];
        var developer = new HashSet<string>(["Cooker"], StringComparer.Ordinal);
        IReadOnlyList<ReleaseViolation> Analyze(string[] selected, NativePlatform platform) =>
            ReleaseExclusion.Analyze(modules, developer, [("Game", selected)], platform);

        // 양성 대조: Runtime 모듈만 닿는 Game 타깃.
        Verify.Equal(0, Analyze(["Game"], NativePlatform.Windows).Count, "clean Release target rejected");

        // Developer 모듈 도달.
        IReadOnlyList<ReleaseViolation> reached = Analyze(["Game", "Cooker"], NativePlatform.Android);
        Verify.That(reached.Count == 1 && reached[0].Rule == "release-developer-module" && reached[0].Module == "Cooker",
            "Developer module reached by a Release target escaped: " + string.Join('\n', reached));
        // 실행 중 적재 관계(DynamicModules)로만 닿아도 Monolithic 실행 파일에 링크되므로 같은 위반이다(Docs/Design/ModuleLoader.ko.md 3.8절).
        NativeModuleInput[] dynamicModules = [.. modules.Select(module => module.Name == "Engine" ? module with { DynamicModules = ["Cooker"] } : module)];
        IReadOnlyList<ReleaseViolation> loaded = ReleaseExclusion.Analyze(dynamicModules, developer, [("Game", ["Game"])], NativePlatform.Windows);
        Verify.That(loaded.Count == 1 && loaded[0].Rule == "release-developer-module" && loaded[0].Module == "Cooker",
            "Developer module loaded at run time by a Release target escaped: " + string.Join('\n', loaded));

        // 셰이더 컴파일러 링크(시스템 라이브러리·prebuilt).
        IReadOnlyList<ReleaseViolation> compilers = Analyze(["Game", "D3D12Graphics", "Dxc"], NativePlatform.MacOS);
        Verify.That(compilers.Count == 2 && compilers.All(violation => violation.Rule == "release-shader-compiler") &&
            compilers.Any(violation => violation.Item == "d3dcompiler") && compilers.Any(violation => violation.Item == "dxcompiler"),
            "shader compiler link escaped: " + string.Join('\n', compilers));

        // Windows Release 대상의 OpenGLGraphics 링크. Android는 GLES backend로 링크한다.
        IReadOnlyList<ReleaseViolation> opengl = Analyze(["Game", "OpenGLGraphics"], NativePlatform.Windows);
        Verify.That(opengl.Count == 1 && opengl[0].Rule == "release-windows-opengl", "Windows OpenGL link escaped: " + string.Join('\n', opengl));
        Verify.Equal(0, Analyze(["Game", "OpenGLGraphics"], NativePlatform.Android).Count, "Android OpenGL ES link rejected");

        // 알려진 예외는 실패가 아니라 알림이고, 맞는 위반이 없는 예외는 실패다(SH2 뒤 지우게 한다).
        ReleaseException known = new("release-shader-compiler", NativePlatform.MacOS, "D3D12Graphics", "d3dcompiler", "SH2", "fixture");
        ReleaseExclusionResult applied = ReleaseExclusion.Apply(compilers, [known]);
        Verify.That(applied.Failures.Count == 1 && applied.Failures[0].Rule == "release-shader-compiler" && applied.Notices.Count == 1 &&
            applied.Notices[0].Contains("until SH2", StringComparison.Ordinal), "known exception handling: " + string.Join('\n', applied.Failures));
        ReleaseExclusionResult stale = ReleaseExclusion.Apply([], [known]);
        Verify.That(stale.Failures.Count == 1 && stale.Failures[0].Rule == "release-exception-stale", "stale exception escaped");

        // Developer 판정은 경로가 아니라 모듈 종류 칸이다(Docs/Design/BuildTargets 3.9절 BD38). 플러그인 경로의 Developer 모듈은
        // Source/Developer가 없어도 잡히고(음성 대조: 예전 경로 판정이 놓치던 모양), 종류가 Runtime이면 Source/Developer 경로라도 아니다.
        NativeModuleInput importer = new()
        {
            Name = "Importer",
            Type = Rocks.Build.Descriptors.ModuleType.Developer,
            ModuleDirectory = "Engine/Plugins/Importers/Importer/Source/Importer"
        };
        NativeModuleInput[] typed = [.. modules, importer];
        IReadOnlySet<string> typedDeveloper = ReleaseExclusion.DeveloperModules(typed);
        Verify.That(typedDeveloper.SetEquals(["Importer"]), "Developer modules by type: " + string.Join(',', typedDeveloper));
        IReadOnlyList<ReleaseViolation> plugin = ReleaseExclusion.Analyze(typed, typedDeveloper, [("Game", ["Game", "Importer"])], NativePlatform.Windows);
        Verify.That(plugin.Count == 1 && plugin[0].Rule == "release-developer-module" && plugin[0].Module == "Importer",
            "plugin Developer module reached by a Release target escaped: " + string.Join('\n', plugin));

        // 종류 칸과 Group 폴더가 다르면(Configure 재정의가 종류를 잃음) module-type-group이다. 플러그인 경로는 기술자가 정하므로 보지 않는다.
        var directories = new Dictionary<string, string>(StringComparer.Ordinal) { ["Cooker"] = "Engine/Source/Developer/Cooker" };
        IReadOnlyList<CheckFailure> mismatches = ReleaseExclusion.GroupMismatches(typed, directories);
        Verify.That(mismatches.Count == 1 && mismatches[0].Rule == "module-type-group" && mismatches[0].Path == "module Cooker",
            "type and group mismatch escaped: " + string.Join('\n', mismatches));
        NoFailures(ReleaseExclusion.GroupMismatches([.. typed.Where(module => module.Name != "Cooker"),
            new() { Name = "Cooker", Type = Rocks.Build.Descriptors.ModuleType.Developer }], directories));
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));

    private sealed class DefaultTarget : TargetRules
    {
        public override string Name => "Fixture";

        public override IReadOnlyList<string> Modules => ["Game"];
    }
}
