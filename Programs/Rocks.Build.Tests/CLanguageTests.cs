using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// C 번역 단위 지원(외부 라이브러리 원본용)을 검사한다.
/// <list type="bullet">
/// <item>C++ 번역 단위 인자는 C 지원 이전과 byte 단위로 같다(golden). 바뀌면 모든 증분 빌드가 무효화된다.</item>
/// <item>.c는 C17로 컴파일하고 경고·FP·구성 프로필은 C++와 같으며 C++ 전용 플래그만 빠진다.</item>
/// <item>실제 toolchain으로 C 정적 라이브러리를 C++ 실행 파일에 링크해 실행한다.</item>
/// <item>다른 .c를 include하는 번역 단위(LZ4 lz4hc.c 방식)는 include된 .c가 바뀌면 다시 컴파일한다.</item>
/// </list>
/// </summary>
internal static class CLanguageTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        NativeToolchain toolchain = request.Toolchain;
        bool windows = toolchain.Platform == NativePlatform.Windows;

        // 1. C++ golden(Debug, 기본 최적화). C 지원을 추가하며 순서·값이 하나라도 바뀌면 실패한다.
        string[] cppGolden = toolchain.Platform switch
        {
            NativePlatform.Windows =>
            [
                "/nologo", "/utf-8", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-std=c++23",
                "/clang:-ffp-contract=off", "/DWIN32", "/D_WINDOWS", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
                "/DROCKS_PLATFORM_WINDOWS=1", "/DROCKS_BUILD_DEBUG=1", "/MDd", "/Od", "/Z7"
            ],
            NativePlatform.Android =>
            [
                "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti", "-fno-exceptions", "-ffp-contract=off",
                "-fPIC", "-DROCKS_PLATFORM_ANDROID=1", "-DROCKS_BUILD_DEBUG=1", "-O0", "-g"
            ],
            _ =>
            [
                "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti", "-fno-exceptions", "-ffp-contract=off",
                "-DROCKS_PLATFORM_MAC=1", "-DROCKS_BUILD_DEBUG=1", "-O0", "-g"
            ]
        };
        string[] cpp = ActionGraph.CompileFlags(toolchain, NativeConfiguration.Debug).ToArray();
        Check(cpp.SequenceEqual(cppGolden), "C++ compile flags changed:\n" + string.Join(' ', cpp));

        // compile action 전체(toolchain 인자 + 프로필 + LTO + 정의 + include + 꼬리)도 Debug·Release에서 고정한다.
        string[] releaseProfile = toolchain.Platform switch
        {
            NativePlatform.Windows =>
            [
                "/nologo", "/utf-8", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-std=c++23",
                "/clang:-ffp-contract=off", "/DWIN32", "/D_WINDOWS", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
                "/DROCKS_PLATFORM_WINDOWS=1", "/DROCKS_BUILD_RELEASE=1", "/MD", "/O2", "/DNDEBUG", "/Z7", "-flto=thin"
            ],
            NativePlatform.Android =>
            [
                "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti", "-fno-exceptions", "-ffp-contract=off",
                "-fPIC", "-DROCKS_PLATFORM_ANDROID=1", "-DROCKS_BUILD_RELEASE=1", "-O2", "-DNDEBUG", "-g", "-flto=thin"
            ],
            _ =>
            [
                "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti", "-fno-exceptions", "-ffp-contract=off",
                "-DROCKS_PLATFORM_MAC=1", "-DROCKS_BUILD_RELEASE=1", "-O2", "-DNDEBUG", "-g", "-flto=thin"
            ]
        };
        NativeModuleInput engine = request.Modules.Single(module => module.Name == "Engine");
        string engineSource = Path.GetFullPath(engine.Sources.Single(), root);
        string headerInclude = Path.GetFullPath(request.Modules.Single(module => module.Name == "Headers").PublicIncludeDirectories.Single(), root);
        string[] tail = windows
            ? ["/I" + headerInclude, "/c", "/TP", engineSource, "/Fo{OUTPUT0}"]
            : ["-I" + headerInclude, "-c", engineSource, "-o", "{OUTPUT0}"];
        foreach ((NativeConfiguration configuration, string[] profile) in new[] { (NativeConfiguration.Debug, cppGolden), (NativeConfiguration.Release, releaseProfile) })
        {
            NativeAction action = ActionGraph.Create(request with { Configuration = configuration }).Actions
                .Single(candidate => candidate.Kind == "Compile" && candidate.Module == "Engine");
            string[] expected = [.. toolchain.CompileArguments, .. profile, .. tail];
            Check(action.Arguments.SequenceEqual(expected),
                $"{configuration} C++ compile action changed:\n{string.Join(' ', action.Arguments)}\nexpected:\n{string.Join(' ', expected)}");
        }

        // 번역 단위 언어·표준은 빌드 도구가 확장자로 정한다. 모듈 인자로 바꾸지 못한다.
        foreach (string argument in new[] { "-x", "-xc", "-xc++", "--language=c", "-std=c11", "/std:c++20", "/TC", "/TP", "/Tcfoo.c", "/Tpfoo.cpp" })
        {
            try
            {
                InputValidation.Validate(request with
                {
                    Modules = request.Modules.Select(module => module.Name == "Engine" ? module with { AdditionalCompileArguments = [argument] } : module).ToArray()
                });
                throw new InvalidOperationException("language-switching module argument accepted: " + argument);
            }
            catch (ArgumentException)
            {
            }
        }

        Check(ActionGraph.CompileFlags(toolchain, NativeConfiguration.Debug, CodeOptimization.Default, NativeLanguage.Cpp)
            .SequenceEqual(cppGolden), "explicit C++ language must equal the default");

        // 2. C 플래그: C17, C++ 전용 플래그 없음, 공통 경고·구성 플래그 유지.
        string[] c = ActionGraph.CompileFlags(toolchain, NativeConfiguration.Debug, CodeOptimization.Default, NativeLanguage.C).ToArray();
        string[] cppOnly = windows ? ["/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-std=c++23"] : ["-fno-rtti", "-fno-exceptions", "-std=c++23"];
        Check(c.Contains(windows ? "/clang:-std=c17" : "-std=c17"), "C translation units must use C17");
        Check(!c.Intersect(cppOnly).Any(), "C translation units must not receive C++-only flags: " + string.Join(' ', c));
        Check(cppGolden.Except(cppOnly).SequenceEqual(c.Where(flag => flag is not ("/clang:-std=c17" or "-std=c17"))),
            "C flags must equal the C++ profile minus C++-only flags:\n" + string.Join(' ', c));
        Check(ActionGraph.LanguageOf("a.c") == NativeLanguage.C && ActionGraph.LanguageOf("a.C") == NativeLanguage.Cpp &&
              ActionGraph.LanguageOf("a.cpp") == NativeLanguage.Cpp, "language must follow the exact lowercase .c extension");

        // 3. 실제 빌드: C 라이브러리(다른 .c를 include) + C++ 실행 파일.
        string fixture = Directory.CreateDirectory(Path.Combine(root, "c-language")).FullName;
        string libraryDirectory = Directory.CreateDirectory(Path.Combine(fixture, "clib")).FullName;
        string appDirectory = Directory.CreateDirectory(Path.Combine(fixture, "app")).FullName;
        string unity = Path.Combine(libraryDirectory, "unity.c");
        string included = Path.Combine(libraryDirectory, "impl.c");
        string main = Path.Combine(appDirectory, "main.cpp");
        File.WriteAllText(included, "static int Twice(int value) { return value * 2; }\n");
        // C 전용 문법(지정 초기화·복합 리터럴·_Static_assert)으로 실제 C 모드인지 확인한다.
        File.WriteAllText(unity,
            "#include \"impl.c\"\n" +
            "struct Pair { int first; int second; };\n" +
            "_Static_assert(sizeof(struct Pair) == 2 * sizeof(int), \"C struct layout\");\n" +
            "int RocksCValue(void) { struct Pair pair = { .second = 3, .first = 4 }; return Twice((&(struct Pair){ pair.first, pair.second })->first); }\n");
        File.WriteAllText(main, "extern \"C\" int RocksCValue(void);\nint main() { return RocksCValue() == 8 ? 0 : 3; }\n");
        NativeBuildRequest cRequest = request with
        {
            RootDirectory = root,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules =
            [
                new() { Name = "CLib", Sources = [unity], Optimization = CodeOptimization.Always },
                new() { Name = "CApp", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["CLib"] }
            ],
            Targets = ["CApp"]
        };
        NativeAction compile = ActionGraph.Create(cRequest).Actions.Single(action => action.Kind == "Compile" && action.Module == "CLib");
        if (windows)
        {
            Check(compile.Arguments.Contains("/TC") && !compile.Arguments.Contains("/TP"), "C compile must use /TC");
        }
        else
        {
            int index = compile.Arguments.ToList().IndexOf(unity);
            Check(index >= 2 && compile.Arguments[index - 2] == "-x" && compile.Arguments[index - 1] == "c",
                "POSIX C compile must place -x c before the source (clang++ driver)");
        }

        NativeAction cppCompile = ActionGraph.Create(cRequest).Actions.Single(action => action.Kind == "Compile" && action.Module == "CApp");
        Check(!cppCompile.Arguments.Contains("/TC") && !cppCompile.Arguments.Contains("-x"), "C++ compile must not switch language");

        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Build()
        {
            var result = await builder.BuildAsync(cRequest);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException("C fixture build failed: " + result.Failure + "\n" +
                    string.Join("\n", result.Actions.Select(action => action.StdOut + action.StdErr)));
            }

            return result;
        }

        var cold = await Build();
        if (toolchain.Platform != NativePlatform.Android)
        {
            var run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["CApp"], [], root, toolchain.Environment));
            Check(run.ExitCode == 0, "C fixture executable returned " + run.ExitCode);
        }

        Check((await Build()).ExecutedActions == 0, "unchanged C fixture rebuilt");

        // 4. include된 .c가 바뀌면 그 번역 단위를 다시 컴파일한다.
        File.WriteAllText(included, "static int Twice(int value) { return value + value; }\n");
        var changed = await Build();
        Check(changed.Actions.Any(action => action.Module == "CLib" && action.Kind == "Compile" && action.Executed),
            "included .c change did not rebuild the including translation unit");

        // 5. Release: C 번역 단위도 ThinLTO bitcode로 만들어 C++ 실행 파일과 링크한다.
        var release = await builder.BuildAsync(cRequest with
        {
            Configuration = NativeConfiguration.Release,
            OutputDirectory = Path.Combine(fixture, "out-release")
        });
        if (!release.Succeeded)
        {
            throw new InvalidOperationException("C fixture Release build failed: " + release.Failure + "\n" +
                string.Join("\n", release.Actions.Select(action => action.StdOut + action.StdErr)));
        }

        if (toolchain.Platform != NativePlatform.Android)
        {
            var run = await ProcessRunner.RunAsync(new(release.ModuleOutputs["CApp"], [], root, toolchain.Environment));
            Check(run.ExitCode == 0, "C fixture Release executable returned " + run.ExitCode);
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
