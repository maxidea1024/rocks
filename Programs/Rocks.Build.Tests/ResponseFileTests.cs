using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 긴 명령줄의 response file(NativeCommandLine·ResponseFile)을 검사한다: 두 인용 규칙의 왕복(LLVM 토크나이저를 옮긴 참조 구현),
/// 결정적 byte·인코딩, Windows 명령줄 길이 = 실제 CreateProcess 명령줄, 도구별 규칙 선택과 호스트 한계 경계, 실제 도구로 강제한
/// compile·archive·link(공백·큰따옴표·역슬래시·한글), key·compile DB 불변, 임시 파일 정리, 실패 진단의 펼친 명령, 호스트 한계를 넘는
/// 실제 긴 경로 빌드.
/// </summary>
internal static class ResponseFileTests
{
    // 공백·탭·큰따옴표·작은따옴표·역슬래시(따옴표 앞·끝)·UNC·한글·줄바꿈·빈 인자.
    private static readonly string[] TrickyArguments =
    [
        "plain", "", "a b", "tab\there", "quote\"inside", "\"", "\\\"", "trail\\", "trail space\\", @"C:\dir with space\",
        @"\\server\share\x y", "multi\\\\\\\"q", "it's", "'single quoted'", "한글 경로\\파일.cpp", "line\nbreak", "/DNAME=\"a b\\\\c\"",
        "-DPATH=C:\\x\\", "  leading", "trailing  "
    ];

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        Quoting();
        pass("response files: Windows/GNU quoting round-trips through the LLVM tokenizers, deterministic bytes and encodings");

        await CommandLineLengthAsync(request, root);
        pass("response files: Windows command line length equals the real CreateProcess command line");

        Selection();
        pass("response files: per-tool syntax (clang-cl, clang++, llvm-lib, llvm-ar, Apple ar) and the host limit boundary");

        await ForcedBuildAsync(request, root);
        pass("response files: forced compile/archive/link with real tools, unchanged keys and compile DB, cleanup, expanded failure command");

        bool android = await AndroidForcedBuildAsync(request, root);
        pass(android ? "response files: forced Android NDK clang++ (GNU) and llvm-ar (host rules) build"
            : "response files: forced Android NDK build skipped (NDK not found)");

        await LongCommandLineAsync(request, root);
        pass("response files: a deep-path build whose compile and link command lines exceed the host limit");
    }

    private enum WindowsState
    {
        Initial,
        Unquoted,
        Quoted
    }

    private static void Quoting()
    {
        foreach (ResponseFileQuoting quoting in new[] { ResponseFileQuoting.Windows, ResponseFileQuoting.Gnu })
        {
            // GNU 규칙은 빈 인자를 나타낼 수 없다(LLVM이 빈 토큰을 버린다) — 아래에서 거부를 따로 본다.
            string[] cases = quoting == ResponseFileQuoting.Gnu ? TrickyArguments.Where(value => value.Length != 0).ToArray() : TrickyArguments;
            string text = ResponseFile.Format(cases, quoting);
            List<string> parsed = quoting == ResponseFileQuoting.Windows ? TokenizeWindows(text) : TokenizeGnu(text);
            Check(parsed.SequenceEqual(cases, StringComparer.Ordinal),
                $"{quoting} response file does not round-trip:\n{text}\n=> {string.Join(" | ", parsed)}");
        }

        // 결정적 golden: 같은 인자면 언제나 이 문자열이다(경로·시각을 담지 않는다).
        string[] golden = [@"C:\a b\", "x\"y", "한", "-DV='q'"];
        Check(ResponseFile.Format(golden, ResponseFileQuoting.Windows) == @"""C:\a b\\"" ""x\""y"" 한 -DV='q'",
            "Windows response file golden changed: " + ResponseFile.Format(golden, ResponseFileQuoting.Windows));
        Check(ResponseFile.Format(golden, ResponseFileQuoting.Gnu) == "\"C:\\\\a b\\\\\"\n\"x\\\"y\"\n한\n\"-DV='q'\"\n",
            "GNU response file golden changed: " + ResponseFile.Format(golden, ResponseFileQuoting.Gnu));

        // Windows는 한 줄(clang-cl의 /link가 줄 끝에서 끊긴다) UTF-16LE BOM, GNU는 인자마다 LF 줄 BOM 없는 UTF-8이다.
        byte[] windows = ResponseFile.Encode(["/c", "a b.cpp", "/link", "/OUT:x.exe"], ResponseFileQuoting.Windows);
        Check(windows[0] == 0xFF && windows[1] == 0xFE && Encoding.Unicode.GetString(windows, 2, windows.Length - 2) == "/c \"a b.cpp\" /link /OUT:x.exe",
            "Windows response file must be one UTF-16LE line with a BOM");
        byte[] gnu = ResponseFile.Encode(["-c", "한 글.cpp"], ResponseFileQuoting.Gnu);
        Check(Encoding.UTF8.GetString(gnu) == "-c\n\"한 글.cpp\"\n" && gnu[0] == (byte)'-', "GNU response file must be LF lines of UTF-8 without a BOM");

        // 규칙별 golden: 그대로 둘 인자와 인용할 인자.
        Check(ResponseFile.Quote("plain", ResponseFileQuoting.Windows) == "plain" && ResponseFile.Quote("plain", ResponseFileQuoting.Gnu) == "plain",
            "plain arguments must stay unquoted");
        Check(ResponseFile.Quote(@"C:\x", ResponseFileQuoting.Windows) == @"C:\x", "Windows backslashes outside quotes are literal");
        Check(ResponseFile.Quote(@"C:\x", ResponseFileQuoting.Gnu) == "\"C:\\\\x\"", "GNU backslashes must be escaped");
        Check(ResponseFile.Quote(@"C:\a b\", ResponseFileQuoting.Windows) == "\"C:\\a b\\\\\"", "Windows trailing backslash must be doubled before the closing quote");
        Check(ResponseFile.Quote("it's", ResponseFileQuoting.Windows) == "it's" && ResponseFile.Quote("it's", ResponseFileQuoting.Gnu) == "\"it's\"",
            "single quotes are literal for Windows and quoted for GNU");
        Check(ResponseFile.Quote("", ResponseFileQuoting.Windows) == "\"\"", "Windows empty arguments must stay as an empty token");
        (string Argument, ResponseFileQuoting Quoting)[] unrepresentable =
        [
            ("a\0b", ResponseFileQuoting.Windows), ("a\0b", ResponseFileQuoting.Gnu), ("", ResponseFileQuoting.Gnu)
        ];
        foreach ((string argument, ResponseFileQuoting quoting) in unrepresentable)
        {
            try
            {
                ResponseFile.Quote(argument, quoting);
                throw new InvalidOperationException($"Unrepresentable {quoting} argument accepted");
            }
            catch (ArgumentException) { }
        }
    }

    private static async Task CommandLineLengthAsync(NativeBuildRequest request, string root)
    {
        if (!OperatingSystem.IsWindows())
        {
            // POSIX 인자 영역은 UTF-8 byte와 끝 NUL의 합이다.
            Check(ResponseFile.PosixArgumentBytes("/usr/bin/clang++", ["-c", "한"]) == "/usr/bin/clang++".Length + 1 + 3 + 3 + 1,
                "POSIX argument bytes must count UTF-8 bytes and terminators");
            return;
        }

        // 자식이 GetCommandLineW로 읽은 실제 명령줄 길이와 계산이 같아야 한계 판정이 정확하다.
        string dotnet = NativeEnvironment.FindExecutable("dotnet", request.Toolchain.Environment);
        string[] arguments = [Assembly.GetExecutingAssembly().Location, "--print-command-line", .. TrickyArguments.Where(value => !value.Contains('\n'))];
        ProcessResult child = await ProcessRunner.RunAsync(new(dotnet, arguments, root, request.Toolchain.Environment));
        Check(child.ExitCode == 0 && child.StdOut.Trim() == ResponseFile.WindowsCommandLineLength(dotnet, arguments).ToString(System.Globalization.CultureInfo.InvariantCulture),
            $"Windows command line length differs from CreateProcess: child={child.StdOut.Trim()} computed={ResponseFile.WindowsCommandLineLength(dotnet, arguments)} {child.StdErr}");
    }

    private static void Selection()
    {
        NativeToolchain Toolchain(NativePlatform platform, string compiler, string archiver) => new()
        {
            Platform = platform,
            CompilerPath = compiler,
            ArchiverPath = archiver,
            Identity = "fixture"
        };
        NativeAction Action(string kind, string tool) => new("fixture:" + kind, "Fixture", kind, tool, [], ["out"], [], [], [], [], []);
        ResponseFileQuoting hostArchiver = OperatingSystem.IsWindows() ? ResponseFileQuoting.Windows : ResponseFileQuoting.Gnu;

        var windows = Toolchain(NativePlatform.Windows, @"C:\llvm\bin\clang-cl.exe", @"C:\llvm\bin\llvm-lib.exe");
        Check(NativeCommandLine.QuotingOf(windows, Action("Compile", windows.CompilerPath)) == ResponseFileQuoting.Windows &&
            NativeCommandLine.QuotingOf(windows, Action("Link", windows.CompilerPath)) == ResponseFileQuoting.Windows &&
            NativeCommandLine.QuotingOf(windows, Action("Archive", windows.ArchiverPath)) == ResponseFileQuoting.Windows,
            "clang-cl and llvm-lib use Windows quoting");
        Check(NativeCommandLine.QuotingOf(windows, Action("Compile", @"C:\other\fake.exe")) is null &&
            NativeCommandLine.QuotingOf(windows, Action("Copy", "")) is null, "unknown tools and copies must not use response files");

        var android = Toolchain(NativePlatform.Android, "/ndk/bin/clang++", "/ndk/bin/llvm-ar");
        Check(NativeCommandLine.QuotingOf(android, Action("Compile", android.CompilerPath)) == ResponseFileQuoting.Gnu &&
            NativeCommandLine.QuotingOf(android, Action("Archive", android.ArchiverPath)) == hostArchiver,
            "NDK clang++ uses GNU quoting and llvm-ar uses the host default");

        var apple = Toolchain(NativePlatform.MacOS, "/usr/bin/clang++", "/usr/bin/ar");
        var homebrew = Toolchain(NativePlatform.MacOS, "/opt/llvm/bin/clang++", "/opt/llvm/bin/llvm-ar");
        Check(NativeCommandLine.QuotingOf(apple, Action("Link", apple.CompilerPath)) == ResponseFileQuoting.Gnu &&
            NativeCommandLine.QuotingOf(apple, Action("Archive", apple.ArchiverPath)) is null &&
            NativeCommandLine.QuotingOf(homebrew, Action("Archive", homebrew.ArchiverPath)) == hostArchiver,
            "Apple ar has no response file syntax; llvm-ar uses the host default");

        // 호스트 한계의 경계: Windows는 명령줄 + NUL이 32,767 이하면 그대로, 하나라도 넘으면 response file이다.
        string tool = OperatingSystem.IsWindows() ? @"C:\t\x.exe" : "/t/x";
        long room = OperatingSystem.IsWindows()
            ? ResponseFile.WindowsCommandLineLimit - 1 - (tool.Length + 2) - 1
            : NativeCommandLine.PosixArgumentLimit - (tool.Length + 1) - 1;
        string[] fits = [new string('a', (int)room)];
        string[] over = [new string('a', (int)room + 1)];
        Check(!NativeCommandLine.ExceedsHostLimit(tool, fits) && NativeCommandLine.ExceedsHostLimit(tool, over),
            "host limit boundary is off by one");
        Check(NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Auto, ResponseFileQuoting.Gnu, tool, fits) is null &&
            NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Auto, ResponseFileQuoting.Gnu, tool, over) == ResponseFileQuoting.Gnu &&
            NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Auto, null, tool, over) is null &&
            NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Always, ResponseFileQuoting.Windows, tool, ["x"]) == ResponseFileQuoting.Windows &&
            NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Always, null, tool, ["x"]) is null &&
            NativeCommandLine.ResponseFileSyntax(ResponseFileMode.Never, ResponseFileQuoting.Windows, tool, over) is null,
            "response file mode selection");
        if (OperatingSystem.IsWindows())
        {
            // 배치 파일 wrapper는 cmd.exe가 실행하므로 cmd.exe 한계(cmd.exe 몫을 남긴 기준)로 판정한다.
            string batch = @"C:\t\clang-cl-wrapper.cmd";
            int batchRoom = NativeCommandLine.WindowsBatchCommandLineLimit - 1 - (batch.Length + 2) - 1;
            Check(!NativeCommandLine.ExceedsHostLimit(batch, [new string('a', batchRoom)]) &&
                NativeCommandLine.ExceedsHostLimit(batch, [new string('a', batchRoom + 1)]), "batch file tools must use the cmd.exe limit");
        }
    }

    // 실제 도구로 모든 action을 response file로 실행한다. 경로(픽스처 루트)에 공백·한글이, define에 공백·큰따옴표·역슬래시·한글이 있다.
    private static async Task ForcedBuildAsync(NativeBuildRequest request, string root)
    {
        string source = Directory.CreateDirectory(Path.Combine(root, "rsp src 한글")).FullName;
        string library = Path.Combine(source, "lib.cpp");
        string main = Path.Combine(source, "main.cpp");
        File.WriteAllText(library, "int RspAnswer() noexcept { return 7; }\n");
        const string mainText = "int RspAnswer() noexcept;\n" +
            "static constexpr char Text[] = ROCKS_RSP_TEXT;\n" +
            "int main() { return RspAnswer() == 7 && sizeof(Text) == 10 && Text[1] == ' ' && Text[3] == '\\\\' && Text[4] == 'c' ? 0 : 3; }\n";
        File.WriteAllText(main, mainText);
        var forced = request with
        {
            OutputDirectory = Path.Combine(root, "rsp out 한글"),
            Modules =
            [
                new() { Name = "RspLib", Sources = [library] },
                new() { Name = "RspGame", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["RspLib"],
                    PrivateDefines = ["ROCKS_RSP_TEXT=\"a b\\\\c 한\""] }
            ],
            Targets = ["RspGame"],
            ResponseFiles = ResponseFileMode.Always
        };
        var builder = new NativeBuildEngine();
        NativeBuildResult built = await builder.BuildAsync(forced);
        Check(built.Succeeded && built.ExecutedActions == 4, "Forced response file build failed: " + Describe(built));
        ProcessResult run = await ProcessRunner.RunAsync(new(built.ModuleOutputs["RspGame"], [], root, request.Toolchain.Environment));
        Check(run.ExitCode == 0, $"Executable built through response files returned {run.ExitCode}: define quoting changed");
        string temporary = Path.Combine(forced.OutputDirectory, ".native-tmp");
        Check(!Directory.EnumerateFileSystemEntries(temporary).Any(), "Response files or staging directories were retained");
        string database = File.ReadAllText(built.CompilationDatabasePath);
        using (JsonDocument document = JsonDocument.Parse(database))
        {
            Check(document.RootElement.EnumerateArray().SelectMany(entry => entry.GetProperty("arguments").EnumerateArray())
                .All(argument => !argument.GetString()!.StartsWith('@')), "compile_commands.json must keep expanded arguments");
        }

        // key는 response file 사용 여부와 무관하다: 같은 출력에서 Auto·Never로 다시 빌드하면 0 액션이고 compile DB도 같다.
        foreach (ResponseFileMode mode in new[] { ResponseFileMode.Auto, ResponseFileMode.Never })
        {
            NativeBuildResult again = await builder.BuildAsync(forced with { ResponseFiles = mode });
            Check(again.Succeeded && again.ExecutedActions == 0, $"Response file mode {mode} changed action keys: " + Describe(again));
            Check(File.ReadAllText(again.CompilationDatabasePath) == database, $"Response file mode {mode} changed compile_commands.json");
        }

        // action마다 실제로 @파일 하나로 실행하고, 그 파일이 펼친 인자의 결정적 인코딩이며 그 뒤 지워지는지 본다. 문법을 모르는
        // 도구(macOS의 Apple ar)는 강제해도 펼친 인자를 그대로 받는다.
        NativePlan plan = ActionGraph.Create(forced with { RootDirectory = Path.GetFullPath(forced.RootDirectory) });
        string toolchainKey = "response-file-fixture";
        var store = new IncrementalStore(Directory.CreateDirectory(Path.Combine(root, "rsp store")).FullName);
        foreach (NativeAction action in plan.Actions.Where(action => action.Kind is "Compile" or "Archive" or "Link"))
        {
            ResponseFileQuoting? quoting = NativeCommandLine.QuotingOf(forced.Toolchain, action);
            Check(quoting is not null || (action.Kind == "Archive" && OperatingSystem.IsMacOS()), "No response file syntax for " + action.Id);
            bool checkedFile = false;
            async Task<ProcessResult> Capture(ProcessSpec spec, CancellationToken cancellationToken)
            {
                if (quoting is null)
                {
                    Check(spec.Arguments.Count == action.Arguments.Count && !spec.Arguments.Any(argument => argument.StartsWith('@')),
                        action.Id + " must receive its expanded arguments when the tool has no response file syntax");
                    checkedFile = true;
                    return await ProcessRunner.RunAsync(spec, cancellationToken);
                }

                Check(spec.Arguments.Count == 1 && spec.Arguments[0].StartsWith('@') &&
                    Path.GetFileName(spec.Arguments[0]) == NativeCommandLine.ResponseFileName, action.Id + " did not run through one response file");
                string file = spec.Arguments[0][1..];
                byte[] bytes = File.ReadAllBytes(file);
                string staged = Path.GetDirectoryName(file)!;
                string[] expanded = action.ResolveArguments(action.Outputs.Select(output => Path.Combine(staged, Path.GetFileName(output))).ToArray());
                Check(bytes.AsSpan().SequenceEqual(ResponseFile.Encode(expanded, quoting.Value)), action.Id + " response file is not the expanded arguments");
                string text = quoting == ResponseFileQuoting.Windows ? Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2) : Encoding.UTF8.GetString(bytes);
                Check((quoting == ResponseFileQuoting.Windows ? TokenizeWindows(text) : TokenizeGnu(text)).SequenceEqual(expanded, StringComparer.Ordinal),
                    action.Id + " response file does not tokenize back to the expanded arguments");
                checkedFile = true;
                return await ProcessRunner.RunAsync(spec, cancellationToken);
            }

            NativeActionResult result = await Executor.RunAsync(forced, action, forced.OutputDirectory, toolchainKey, store,
                CancellationToken.None, Capture);
            Check(result.Succeeded && checkedFile, $"{action.Id} through a captured response file failed: {result.StdErr}");
        }

        Check(!Directory.EnumerateFileSystemEntries(temporary).Any(), "Captured response files were retained");

        // 실패하면 진단에 @파일 대신 펼친 명령이 나오고 임시 response file은 지워진다.
        File.WriteAllText(main, "#error deliberate response file failure\n");
        try
        {
            NativeBuildResult failed = await builder.BuildAsync(forced);
            Check(!failed.Succeeded && failed.Failure is not null && failed.Failure.Contains(NativeCommandLine.ResponseFileName, StringComparison.Ordinal) &&
                failed.Failure.Contains("main.cpp", StringComparison.Ordinal) && failed.Failure.Contains("ROCKS_RSP_TEXT", StringComparison.Ordinal),
                "A failed response file action must show the expanded command: " + failed.Failure);
            Check(!Directory.EnumerateFileSystemEntries(temporary).Any(), "A failed action retained its response file");
        }
        finally
        {
            File.WriteAllText(main, mainText);
        }
    }

    // NDK가 있으면 Android 교차 빌드의 모든 action을 response file로 강제한다: clang++는 호스트와 무관하게 GNU 규칙, llvm-ar는 호스트
    // 규칙이다(실행은 하지 않는다). NDK가 없으면 false다.
    private static async Task<bool> AndroidForcedBuildAsync(NativeBuildRequest request, string root)
    {
        NativeToolchain android;
        try
        {
            android = await NativeToolchains.DiscoverAndroidAsync("arm64", 26);
        }
        catch (FileNotFoundException)
        {
            return false;
        }

        string source = Directory.CreateDirectory(Path.Combine(root, "rsp android 한글")).FullName;
        File.WriteAllText(Path.Combine(source, "lib.cpp"), "int RspAnswer() noexcept { return 7; }\n");
        File.WriteAllText(Path.Combine(source, "main.cpp"), "int RspAnswer() noexcept;\n" +
            "static constexpr char Text[] = ROCKS_RSP_TEXT;\n" +
            "static_assert(sizeof(Text) == 10 && Text[3] == '\\\\');\n" +
            "int main() { return RspAnswer() == 7 ? 0 : 3; }\n");
        var forced = request with
        {
            Toolchain = android,
            OutputDirectory = Path.Combine(root, "rsp android out 한글"),
            Modules =
            [
                new() { Name = "RspLib", Sources = [Path.Combine(source, "lib.cpp")] },
                new() { Name = "RspGame", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(source, "main.cpp")],
                    Dependencies = ["RspLib"], PrivateDefines = ["ROCKS_RSP_TEXT=\"a b\\\\c 한\""] }
            ],
            Targets = ["RspGame"],
            ResponseFiles = ResponseFileMode.Always
        };
        var builder = new NativeBuildEngine();
        NativeBuildResult built = await builder.BuildAsync(forced);
        Check(built.Succeeded && built.ExecutedActions == 4, "Forced Android response file build failed: " + Describe(built));
        Check(!Directory.EnumerateFileSystemEntries(Path.Combine(forced.OutputDirectory, ".native-tmp")).Any(),
            "Android response files were retained");
        NativeBuildResult again = await builder.BuildAsync(forced with { ResponseFiles = ResponseFileMode.Auto });
        Check(again.Succeeded && again.ExecutedActions == 0, "Android response file mode changed action keys: " + Describe(again));
        return true;
    }

    // 깊은 경로에서 include 디렉터리가 많은 compile과 prebuilt 라이브러리가 많은 link가 Windows 한계(32,767자)를 넘어도 Auto로 빌드된다.
    // Never로 강제하면 같은 명령이 시작조차 못 하고 그 이유가 진단에 나온다. POSIX 기준(128 KiB)은 넘지 않으므로 그대로 빌드만 확인한다.
    private static async Task LongCommandLineAsync(NativeBuildRequest request, string root)
    {
        bool windows = OperatingSystem.IsWindows();
        // 경로 하나를 MAX_PATH(260) 아래로 두면서 길게 만든다.
        string deep = Path.Combine(root, "long command line " + new string('d', 40), new string('e', 40));
        string Padded(string prefix, int index)
        {
            string name = prefix + index.ToString("D3", System.Globalization.CultureInfo.InvariantCulture) + "-";
            return name + new string('p', Math.Max(1, 230 - deep.Length - name.Length - 12));
        }

        string source = Directory.CreateDirectory(Path.Combine(deep, "src")).FullName;
        File.WriteAllText(Path.Combine(source, "lib.cpp"), "int LongAnswer() noexcept { return 5; }\n");
        File.WriteAllText(Path.Combine(source, "main.cpp"), "int LongAnswer() noexcept;\nint main() { return LongAnswer() == 5 ? 0 : 4; }\n");
        string[] includes = Enumerable.Range(0, 160).Select(index => Directory.CreateDirectory(Path.Combine(deep, Padded("i", index))).FullName).ToArray();
        var builder = new NativeBuildEngine();
        var libraryRequest = request with
        {
            OutputDirectory = Path.Combine(deep, "out-lib"),
            Modules = [new() { Name = "LongLib", Sources = [Path.Combine(source, "lib.cpp")] }],
            Targets = ["LongLib"]
        };
        NativeBuildResult library = await builder.BuildAsync(libraryRequest);
        Check(library.Succeeded, "Long command line library failed: " + Describe(library));
        // 같은 라이브러리의 사본들이다. 첫 사본이 기호를 풀고 나머지는 링커가 읽기만 한다.
        string libraries = Directory.CreateDirectory(Path.Combine(deep, "l")).FullName;
        string[] prebuilt = Enumerable.Range(0, 150).Select(index =>
        {
            string copy = Path.Combine(libraries, Padded("l", index) + Path.GetExtension(library.ModuleOutputs["LongLib"]));
            File.Copy(library.ModuleOutputs["LongLib"], copy, true);
            return copy;
        }).ToArray();
        var longRequest = request with
        {
            OutputDirectory = Path.Combine(deep, "out"),
            Modules =
            [
                new() { Name = "LongGame", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(source, "main.cpp")],
                    PrivateIncludeDirectories = includes, PrebuiltLibraries = prebuilt }
            ],
            Targets = ["LongGame"]
        };
        NativePlan plan = ActionGraph.Create(longRequest);
        if (windows)
        {
            foreach (NativeAction action in plan.Actions)
            {
                string[] arguments = action.ResolveArguments(action.Outputs);
                Check(NativeCommandLine.ExceedsHostLimit(action.Tool, arguments),
                    $"{action.Id} command line ({ResponseFile.WindowsCommandLineLength(action.Tool, arguments)} characters) does not exceed the Windows limit");
            }

            NativeBuildResult refused = await builder.BuildAsync(longRequest with { OutputDirectory = Path.Combine(deep, "out-never"), ResponseFiles = ResponseFileMode.Never });
            Check(!refused.Succeeded && refused.Failure is not null && refused.Failure.Contains("response files are disabled", StringComparison.Ordinal),
                "Without response files the over-limit command must fail with the limit diagnostic: " + refused.Failure);

            // toolchain 도구가 아니면 문법을 모르므로 그대로 넘기고, 시작하지 못하면 그 이유를 알린다.
            string unknownTool = NativeEnvironment.FindExecutable("dotnet", request.Toolchain.Environment);
            string input = Path.Combine(source, "main.cpp");
            var unknown = new NativeAction("fixture:unknown-tool", "Fixture", "Compile", unknownTool, [new string('a', ResponseFile.WindowsCommandLineLimit)],
                [Path.Combine(deep, "unknown", "out.obj")], [input], [], [], [], []);
            var unknownStore = new IncrementalStore(Directory.CreateDirectory(Path.Combine(deep, "unknown")).FullName);
            NativeActionResult unknownResult = await Executor.RunAsync(longRequest, unknown, Path.Combine(deep, "unknown"), "fixture",
                unknownStore, CancellationToken.None);
            Check(!unknownResult.Succeeded && unknownResult.StdErr.Contains("has no known response file syntax", StringComparison.Ordinal),
                "An over-limit command of a tool without response file syntax must explain the failure: " + unknownResult.StdErr);
        }

        NativeBuildResult built = await builder.BuildAsync(longRequest);
        Check(built.Succeeded && built.ExecutedActions == 2, "Over-limit command lines did not build: " + Describe(built));
        ProcessResult run = await ProcessRunner.RunAsync(new(built.ModuleOutputs["LongGame"], [], root, request.Toolchain.Environment));
        Check(run.ExitCode == 0, "Long command line executable failed: " + run.ExitCode);
        Check((await builder.BuildAsync(longRequest)).ExecutedActions == 0, "Long command line no-op rebuild executed actions");
    }

    // LLVM llvm/lib/Support/CommandLine.cpp tokenizeWindowsCommandLineImpl(인자 부분, 줄 끝 표시 없음)을 옮긴 것이다.
    private static List<string> TokenizeWindows(string source)
    {
        static bool Space(char value) => value is ' ' or '\t' or '\r' or '\n' or '\0';
        var tokens = new List<string>();
        var token = new StringBuilder();
        var state = WindowsState.Initial;
        int ParseBackslash(int index)
        {
            int count = 0;
            do
            {
                ++index;
                ++count;
            }
            while (index != source.Length && source[index] == '\\');

            if (index != source.Length && source[index] == '"')
            {
                token.Append('\\', count / 2);
                if (count % 2 == 0)
                {
                    return index - 1;
                }

                token.Append('"');
                return index;
            }

            token.Append('\\', count);
            return index - 1;
        }

        for (int index = 0; index < source.Length; ++index)
        {
            char value = source[index];
            if (state == WindowsState.Initial)
            {
                if (Space(value))
                {
                    continue;
                }

                state = WindowsState.Unquoted;
            }

            if (state == WindowsState.Unquoted)
            {
                if (Space(value))
                {
                    tokens.Add(token.ToString());
                    token.Clear();
                    state = WindowsState.Initial;
                }
                else if (value == '"')
                {
                    state = WindowsState.Quoted;
                }
                else if (value == '\\')
                {
                    index = ParseBackslash(index);
                }
                else
                {
                    token.Append(value);
                }
            }
            else if (value == '"')
            {
                if (index + 1 < source.Length && source[index + 1] == '"')
                {
                    token.Append('"');
                    ++index;
                }
                else
                {
                    state = WindowsState.Unquoted;
                }
            }
            else if (value == '\\')
            {
                index = ParseBackslash(index);
            }
            else
            {
                token.Append(value);
            }
        }

        if (state != WindowsState.Initial)
        {
            tokens.Add(token.ToString());
        }

        return tokens;
    }

    // LLVM llvm/lib/Support/CommandLine.cpp TokenizeGNUCommandLine(줄 끝 표시 없음)을 옮긴 것이다. 빈 토큰은 버린다(LLVM과 같다).
    private static List<string> TokenizeGnu(string source)
    {
        static bool Space(char value) => value is ' ' or '\t' or '\r' or '\n';
        var tokens = new List<string>();
        var token = new StringBuilder();
        for (int index = 0; index < source.Length; ++index)
        {
            if (token.Length == 0)
            {
                while (index < source.Length && Space(source[index]))
                {
                    ++index;
                }

                if (index == source.Length)
                {
                    break;
                }
            }

            char value = source[index];
            if (index + 1 < source.Length && value == '\\')
            {
                ++index;
                token.Append(source[index]);
                continue;
            }

            if (value is '\'' or '"')
            {
                ++index;
                while (index < source.Length && source[index] != value)
                {
                    if (source[index] == '\\' && index + 1 != source.Length)
                    {
                        ++index;
                    }

                    token.Append(source[index]);
                    ++index;
                }

                if (index == source.Length)
                {
                    break;
                }

                continue;
            }

            if (Space(value))
            {
                if (token.Length != 0)
                {
                    tokens.Add(token.ToString());
                }

                token.Clear();
                continue;
            }

            token.Append(value);
        }

        if (token.Length != 0)
        {
            tokens.Add(token.ToString());
        }

        return tokens;
    }

    private static string Describe(NativeBuildResult result) => result.Failure + "\n" +
        string.Join("\n", result.Actions.Select(action => $"{action.Id}:executed={action.Executed}:{action.StdOut}{action.StdErr}"));

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

/// <summary>자식 프로세스 모드(<c>--print-command-line</c>)가 OS에서 받은 원래 명령줄 길이를 읽는다(Windows 전용).</summary>
internal static class CommandLineProbe
{
    public static int Length() => Marshal.PtrToStringUni(GetCommandLineW())!.Length;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetCommandLineW();
}
