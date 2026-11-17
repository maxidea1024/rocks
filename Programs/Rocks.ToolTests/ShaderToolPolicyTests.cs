using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// 셰이더 도구 정책(Docs/Design/ShaderSystem.ko.md 9절)의 저장소 실행과 음성 대조다: 도구 표 참조(shader-tool-table), binary 로드
// (shader-tool-binary-load), Release 배제(release-*).
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

        // 양성 대조: 메인 프로세스는 표를, 형식 모듈은 표·로더·binary 헤더·절대 경로 열기를, Runtime은 OpenSystem을 쓴다.
        temporary.Write("Engine/Source/Developer/ShaderCompiler/ShaderCompiler.Build.cs", "RequiredModules => [\"ShaderToolTable\"];\n");
        temporary.Write("Engine/Source/Developer/ShaderCompiler/Private/Keys.cpp", "#include <ShaderToolTable/ShaderToolTable.h>\n");
        temporary.Write("Engine/Source/Developer/ShaderFormatD3D/ShaderFormatD3D.Build.cs",
            "RequiredModules => [\"ShaderToolLoader\", \"ShaderToolTable\"]; Headers(\"DirectXShaderCompiler\");\n");
        temporary.Write("Engine/Source/Developer/ShaderFormatD3D/Private/Win64/Dxc.cpp",
            "#include <ShaderToolLoader/ShaderToolLoader.h>\n#include <dxc/dxcapi.h>\nvoid F(platform::DynamicLibrary& l) { (void)l.Open(\"C:/x.dll\"); }\n");
        temporary.Write("Engine/Source/Runtime/Probe/Private/Vulkan.cpp",
            "#include <Platform/DynamicLibrary.h>\nvoid G(platform::DynamicLibrary& l) { (void)l.OpenSystem(\"vulkan-1.dll\"); }\n" +
            "// #include <ShaderToolTable/ShaderToolTable.h> and l.Open(path) in a comment are not code\n");
        temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// depends on \"ShaderToolTable\" only in a comment\nRequiredModules => [\"Core\"];\n");
        // SH0 도구 스파이크는 워커가 생기기 전에 도구를 직접 여는 측정 프로그램이다.
        temporary.Write("Programs/ShaderToolSpike/ShaderToolSpike.Build.cs",
            "Dependencies = [\"ShaderToolLoader\", \"ShaderToolTable\"]; Headers(\"DirectXShaderCompiler\");\n");
        temporary.Write("Programs/ShaderToolSpike/Private/Win64/Dxc.cpp",
            "#include <ShaderToolLoader/ShaderToolLoader.h>\n#include <dxc/dxcapi.h>\n");
        NoFailures(Run());

        (string File, string Content, string Rule)[] cases =
        [
            ("Engine/Source/Runtime/Probe/Private/Table.cpp", "#include <ShaderToolTable/ShaderToolTable.h>\n", "shader-tool-table"),
            ("Engine/Source/Runtime/Probe/Probe.Build.cs", "RequiredModules => [\"ShaderToolTable\"];\n", "shader-tool-table"),
            ("Engine/Source/Runtime/Probe/Probe.Build.cs", "Headers(\"DirectXShaderCompiler\");\n", "shader-tool-binary-load"),
            ("Engine/Source/Runtime/Probe/Private/Dxc.cpp", "#include <dxc/dxcapi.h>\n", "shader-tool-binary-load"),
            ("Engine/Source/Runtime/Probe/Private/Open.cpp",
                "#include <Platform/DynamicLibrary.h>\nvoid H(platform::DynamicLibrary& l, const char* p) { (void)l.Open(p); }\n", "shader-tool-binary-load"),
            // 메인 프로세스(ShaderCompiler)는 표는 쓰지만 binary를 열지 않는다.
            ("Engine/Source/Developer/ShaderCompiler/Private/Load.cpp", "#include <ShaderToolLoader/ShaderToolLoader.h>\n", "shader-tool-binary-load"),
            ("Engine/Source/Developer/ShaderCompiler/ShaderCompiler.Build.cs", "RequiredModules => [\"ShaderToolTable\", \"ShaderToolLoader\"];\n",
                "shader-tool-binary-load"),
            // 이름이 비슷한 폴더는 형식 모듈이 아니다.
            ("Engine/Source/Developer/ShaderFormat/Private/Load.cpp", "#include <ShaderToolLoader/ShaderToolLoader.h>\n", "shader-tool-binary-load"),
            // 스파이크와 이름만 비슷한 프로그램 폴더는 허용 위치가 아니다.
            ("Programs/ShaderToolSpikeOld/Private/Dxc.cpp", "#include <dxc/dxcapi.h>\n", "shader-tool-binary-load")
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
            new() { Name = "Game", Kind = NativeModuleKind.Executable, Dependencies = ["Engine", "Renderer"] },
            new() { Name = "Engine", Dependencies = ["Core"] },
            new() { Name = "Core" },
            new() { Name = "Renderer", Dependencies = ["Engine"] },
            new() { Name = "Cooker", Dependencies = ["Engine"] },
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
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));

    private sealed class DefaultTarget : TargetRules
    {
        public override string Name => "Fixture";

        public override IReadOnlyList<string> Modules => ["Game"];
    }
}
