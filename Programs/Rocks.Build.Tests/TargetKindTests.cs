using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 타깃 종류와 타깃 매크로(Docs/Design/BuildTargets 3.1·3.2절 BS2a)다: 다섯 종류 × Program 속성의 매크로 표, 모든 compile이 같은 값을 받음,
/// 모듈 정의로 ROCKS_WITH_*를 덮어쓰기 거부, Base/Compiler.h의 #error 검사.
/// </summary>
internal static class TargetKindTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, string? repository, Action<string> pass)
    {
        string Row(TargetKind kind) => string.Join(" ", kind.Defines.Select(define => define[^1]));
        Check(Row(new(TargetType.Game)) == "0 0 1 1" && Row(new(TargetType.Editor)) == "1 1 1 1" && Row(new(TargetType.Client)) == "0 0 0 1" &&
            Row(new(TargetType.Server)) == "0 0 1 0" && Row(new(TargetType.Program)) == "0 0 1 1" &&
            Row(new(TargetType.Program, CompileWithEditor: true)) == "1 1 1 1" && Row(new(TargetType.Program, WithEditorData: true)) == "0 1 1 1",
            "target macro table (editor, editor data, server, client)");
        // Program 속성은 Program만 바꾼다(다른 종류의 켬은 규칙 로더가 거부하고, 여기서는 값에 영향이 없다).
        Check(Row(new(TargetType.Server, CompileWithEditor: true, WithEditorData: true)) == "0 0 1 0", "Program properties only affect Program targets");
        Check(TargetKind.Default == new TargetKind(TargetType.Program) &&
            TargetKind.Default.Defines.SequenceEqual(new TargetKind(TargetType.Game).Defines), "the default is the Program default, the Game combination");
        Check(new TargetKind(TargetType.Program).Defines.SequenceEqual(["ROCKS_WITH_EDITOR=0", "ROCKS_WITH_EDITOR_DATA=0", "ROCKS_WITH_SERVER_CODE=1",
            "ROCKS_WITH_CLIENT_CODE=1"]), "define spelling and order");
        pass("target kinds: macro table for the five target types and the Program properties");

        // 모듈 정의로 덮어쓰기 거부(예약 접두).
        NativeModuleInput module = new() { Name = "Probe", Sources = [], PrivateDefines = ["ROCKS_WITH_EDITOR=1"] };
        try
        {
            InputValidation.Validate(request with { Modules = [module], Targets = [] });
            Check(false, "a module define of ROCKS_WITH_EDITOR was accepted");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("reserved for the build configuration and target", StringComparison.Ordinal), error.Message);
        }

        // 모든 compile이 타깃 매크로를 받는다(타깃 전체에 같은 값).
        string[] arguments = [.. ActionGraph.TargetCompileArguments(request with { Target = new(TargetType.Server) }, request.RootDirectory)];
        Check(new TargetKind(TargetType.Server).Defines.All(define => arguments.Contains((request.Toolchain.Platform == NativePlatform.Windows ? "/D" : "-D") + define)),
            "the target compile arguments carry the target macros: " + string.Join(" ", arguments));
        pass("target kinds: reserved ROCKS_WITH_ prefix and target-wide compile arguments");

        // Base/Compiler.h 검사: 빌드가 넘긴 값을 번역 단위가 지우거나 바꾸면 #error다(기본값 없음 — 정의 누락, 0·1 밖, 에디터 코드에 에디터
        // 데이터 없음, 서버·클라이언트 둘 다 없음). 저장소 헤더를 그대로 컴파일한다.
        if (repository is null || request.Toolchain.Platform == NativePlatform.Android)
        {
            return;
        }

        string fixture = Directory.CreateDirectory(Path.Combine(root, "target-kinds")).FullName;
        string basePublic = Path.Combine(repository, "Core", "Source", "Runtime", "Base", "Public");
        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Compile(string name, string prelude)
        {
            string source = Path.Combine(fixture, name, "Probe.cpp");
            Directory.CreateDirectory(Path.GetDirectoryName(source)!);
            File.WriteAllText(source, prelude + "#include <Base/Compiler.h>\nint TargetKindProbe() noexcept { return ROCKS_WITH_CLIENT_CODE; }\n");
            NativeModuleInput probe = new() { Name = "Probe" + name, ModuleDirectory = Path.Combine(fixture, name), Sources = [source], PrivateIncludeDirectories = [basePublic] };
            return await builder.BuildAsync(request with
            {
                OutputDirectory = Path.Combine(fixture, "out-" + name),
                Modules = [probe],
                Targets = [probe.Name],
                Unity = false,
                Pch = false
            });
        }

        NativeBuildResult accepted = await Compile("Accepted", "");
        Check(accepted.Succeeded, "Base/Compiler.h accepts the build's target macros: " + accepted.Failure);
        (string Name, string Prelude, string Expected)[] rejected =
        [
            ("Missing", "#undef ROCKS_WITH_EDITOR\n", "must be defined by Rocks.Build"),
            ("Value", "#undef ROCKS_WITH_SERVER_CODE\n#define ROCKS_WITH_SERVER_CODE 2\n", "must be 0 or 1"),
            ("EditorData", "#undef ROCKS_WITH_EDITOR\n#define ROCKS_WITH_EDITOR 1\n", "requires ROCKS_WITH_EDITOR_DATA"),
            ("NoCode", "#undef ROCKS_WITH_SERVER_CODE\n#undef ROCKS_WITH_CLIENT_CODE\n#define ROCKS_WITH_SERVER_CODE 0\n#define ROCKS_WITH_CLIENT_CODE 0\n",
                "server code, client code or both")
        ];
        foreach ((string name, string prelude, string expected) in rejected)
        {
            NativeBuildResult result = await Compile(name, prelude);
            Check(!result.Succeeded && result.Actions.Any(action => !action.Succeeded && action.StdErr.Contains(expected, StringComparison.Ordinal)),
                $"Base/Compiler.h must reject the {name} case with '{expected}': " + result.Failure + " " +
                string.Join(" | ", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdErr)));
        }

        pass("target kinds: Base/Compiler.h rejects missing, out-of-range and contradictory target macros");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
