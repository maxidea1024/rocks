using Rocks.Build;

/// <summary>
/// Windows SDK 헤더를 감싸는 쌍(<c>Platform/Win64/WindowsHeadersBegin.h</c>·<c>WindowsHeadersEnd.h</c>)이 PCH·강제 include 경계와
/// 쌍 밖에서 먼저 열린 <c>Windows.h</c>를 견디는지 실제 컴파일로 본다(Docs/Design/UnityBuildPch.ko.md 6.1절 "매크로를 남기는 헤더").
/// 쌍은 상태를 저장하지 않아야 한다. clang의 PCH는 <c>#pragma push_macro</c> 스택을 저장하지 않으므로, PCH 안의 End가 저장한 것을 PCH를
/// 쓰는 번역 단위의 Begin이 되돌리는 방식이면 여기서 깨진다. 저장소의 실제 헤더를 쓰므로 저장소 루트가 필요하고, Windows toolchain에서만
/// 돈다.
/// </summary>
internal static class WindowsHeaderPairTests
{
    // 쌍 뒤에서 숨긴 이름을 식별자로 쓰고(매크로가 남아 있으면 컴파일되지 않는다), A/W 이름 매크로가 지워졌는지와 감싼 구간의 COM
    // 메서드가 제 이름으로 선언됐는지 본다(구간을 A/W 매크로가 살아 있는 채로 파싱했다면 메서드는 GetMessageA다).
    private const string Uses =
        "struct Names { int near; int far; int small; int interface; int pascal; int cdecl; int hyper; };\n" +
        "#ifdef GetMessage\n#error \"GetMessage must be undefined after WindowsHeadersEnd.h\"\n#endif\n" +
        "inline int Value(int value) noexcept { return Names{value, 0, 0, 0, 0, 0, 0}.near; }\n" +
        "static_assert(requires { &IDXGIInfoQueue::GetMessage; }, \"COM methods in a wrapped section keep their own names\");\n";

    private const string Begin = "#include <Platform/Win64/WindowsHeadersBegin.h>\n";

    private const string End = "#include <Platform/Win64/WindowsHeadersEnd.h>\n";

    public static async Task RunAsync(NativeBuildRequest request, string root, string? repository, Action<string> pass)
    {
        if (request.Toolchain.Platform != NativePlatform.Windows || repository is null)
        {
            return;
        }

        string fixture = Directory.CreateDirectory(Path.Combine(root, "windows header pair fixture")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
            return path;
        }

        string[] engineIncludes =
        [
            Path.Combine(repository, "Core", "Source", "Runtime", "PlatformBase", "Public"),
            Path.Combine(repository, "Core", "Source", "Runtime", "Base", "Public")
        ];
        // PCH 헤더가 쌍으로 끝난다(숨긴 상태가 PCH에 담긴다). PCH를 쓰는 소스는 두 번째 쌍에서 처음 여는 COM·DirectX 헤더를 파싱한다.
        string pchHeader = Write("pch/PairPch.h", "#pragma once\n" + Begin + "#include <Unknwn.h>\n" + End);
        string[] sources = Enumerable.Range(0, 3).Select(index => Write($"pch/part{index}.cpp",
            "#include \"PairPch.h\"\n" + Begin + "#include <d3d12.h>\n#include <dxgidebug.h>\n" + End +
            $"namespace part{index} {{\n" + Uses + $"}}\nint Part{index}() noexcept {{ return part{index}::Value({index}); }}\n")).ToArray();
        NativeModuleInput withPch = new()
        {
            Name = "PairPch",
            Sources = sources,
            PrivateIncludeDirectories = [Path.GetDirectoryName(pchHeader)!, .. engineIncludes],
            PrivatePchHeader = pchHeader
        };
        // Windows.h가 쌍보다 먼저, 쌍 밖에서 열린 번역 단위다(A/W 이름 매크로가 살아 있는 상태에서 Begin을 만난다).
        string early = Write("early/early.cpp", "#include <Windows.h>\n" + Begin + "#include <dxgidebug.h>\n" + End +
            "namespace early {\n" + Uses + "}\nint Early() noexcept { return early::Value(1); }\n");
        NativeModuleInput withEarlyWindows = new() { Name = "PairEarly", Sources = [early], PrivateIncludeDirectories = engineIncludes };
        NativeBuildRequest build = request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = [withPch, withEarlyWindows],
            Targets = ["PairPch", "PairEarly"]
        };

        NativePlan plan = ActionGraph.Create(build);
        NativeAction pch = plan.Actions.Single(action => action.Kind == "Pch");
        Check(plan.Actions.Count(action => action.Kind == "Compile" && action.Module == "PairPch" && action.Dependencies.Contains(pch.Id)) > 0,
            "the fixture sources must use the PCH (otherwise the PCH boundary is not exercised)");
        await Build(build, "the pair must survive a PCH boundary and a Windows.h opened before it");
        // 유니티·PCH를 끈 검증 변형: PCH 헤더를 강제 include(/FI)로 받는다.
        await Build(build with { Unity = false, Pch = false, OutputDirectory = Path.Combine(fixture, "out-plain") },
            "the pair must survive a forced include boundary");
        pass("Windows header pair survives PCH and forced include boundaries and an earlier Windows.h");
        await RuntimeLibraryRecordAsync(request, fixture, engineIncludes);
        pass("Base/Compiler.h records the MSVC runtime library choice in objects without STL headers");
    }

    // Base/Compiler.h가 CRT 선택을 object에 기록하는지 본다(Docs/Design/UnityBuildPch.ko.md 8.4절 "placement new 헤더" — CRT 선택 기록).
    // STL 헤더를 include하지 않는 번역 단위에는 yvals.h의 기록이 없으므로 이 기록이 링커의 LNK2038 검사를 받친다. clang-cl은
    // #pragma detect_mismatch를 object의 linker directive(/FAILIFMISMATCH:"이름=값")로 쓴다.
    private static async Task RuntimeLibraryRecordAsync(NativeBuildRequest request, string fixture, string[] engineIncludes)
    {
        string source = Path.Combine(fixture, "runtime", "value.cpp");
        Directory.CreateDirectory(Path.GetDirectoryName(source)!);
        File.WriteAllText(source, "#include <Base/Compiler.h>\nint RuntimeValue() noexcept { return 1; }\n");
        NativeModuleInput module = new() { Name = "RuntimeRecord", Sources = [source], PrivateIncludeDirectories = engineIncludes };
        NativeBuildRequest build = request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out-runtime"),
            Modules = [module],
            Targets = ["RuntimeRecord"]
        };
        await Build(build, "the runtime library record fixture must build");
        NativeAction compile = ActionGraph.Create(build).Actions.Single(action => action.Kind == "Compile" && action.Source == source);
        string objectFile = compile.Outputs.First(output => output.EndsWith(".obj", StringComparison.Ordinal));
        // 빌드가 Debug면 /MDd, 그 밖에는 /MD를 넘긴다(ActionGraph).
        string expected = "RuntimeLibrary=" + (request.Configuration == NativeConfiguration.Debug ? "MDd_DynamicDebug" : "MD_DynamicRelease");
        Check(System.Text.Encoding.Latin1.GetString(File.ReadAllBytes(objectFile)).Contains(expected, StringComparison.Ordinal),
            $"the object of a source that includes only Base/Compiler.h must carry the linker directive {expected}");
    }

    private static async Task Build(NativeBuildRequest request, string message)
    {
        NativeBuildResult result = await new NativeBuildEngine().BuildAsync(request);
        Check(result.Succeeded, message + ": " + result.Failure + "\n" +
            string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr)));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
