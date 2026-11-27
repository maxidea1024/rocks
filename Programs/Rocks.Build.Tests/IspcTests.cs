using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// ISPC 원본 컴파일(Docs/Design/BuildTargets 3.12절 BD45–BD50)을 검사한다.
/// <list type="bullet">
/// <item>계획 모양: 고정 인자(아키텍처당 타깃 하나, FMA 끔, fast-math 없음), object가 모듈 archive에 들어감, C++ compile이 Ispc action 뒤에
/// 돌고 생성 헤더를 선언 입력으로 받음, 생성 헤더 디렉터리는 include 디렉터리지만 header 디렉터리가 아님.</item>
/// <item>입력 검증: 같은 basename·Android·확장자, ISPC 없는 요청.</item>
/// <item>-MMM 의존 파일 파서, 버전 판정, 도구 탐색 진단, ISPC 원본이 없는 빌드는 ISPC를 찾지 않음.</item>
/// <item>실제 ISPC로 빌드·링크·실행, 증분(.isph 내용·새 .isph·도구 digest), 생성 헤더 결정성, 도구 빌드 ID 입력.</item>
/// </list>
/// 실제 빌드는 고정 버전 ISPC가 있어야 한다(ROCKS_ISPC 또는 기본 위치 — Build/InstallIspc.ps1). 없으면 실패한다(조용히 건너뛰지 않는다).
/// </summary>
internal static class IspcTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "ispc fixture")).FullName;
        NativeBuildRequest kernel = Fixture(request, fixture, Path.Combine(fixture, "out"));
        FixedArguments();
        pass("ISPC fixed arguments: one target per architecture, FMA off, no fast-math, identity define");

        PlanShape(kernel, fixture);
        pass("ISPC plan: object in the module archive, C++ compile after Ispc with the generated header as declared input");

        Validation(kernel, fixture);
        pass("ISPC validation: same file name, Android, extension, missing compiler");

        DependencyParser(fixture);
        pass("ISPC -MMM dependency parser and version line check");

        await Discovery(request, kernel, fixture);
        pass("ISPC discovery: missing and wrong-version diagnostics, builds without ISPC sources do not look for ISPC");

        IspcTool tool = await IspcTools.DiscoverAsync(root, request.Toolchain.Environment);
        await RealBuild(kernel with { Ispc = tool }, fixture, tool);
        pass("ISPC real build: link and run, incremental .isph and tool digest, deterministic generated header");

        BuildIdInputs(kernel with { Ispc = tool, EngineVersion = new(1, 2, 3) }, fixture);
        pass("ISPC tool build ID: source key, version instead of tool path, same text under another root");
    }

    private static NativeBuildRequest Fixture(NativeBuildRequest template, string fixture, string output)
    {
        Write(fixture, "kernel/k.ispc", "#include \"inc.isph\"\nexport uniform int Twice(uniform int value) { return value * 2 + Offset(); }\n");
        Write(fixture, "kernel/inc.isph", "static inline uniform int Offset() { return 0; }\n");
        Write(fixture, "kernel/user.cpp", "#include \"k_ispc.h\"\nint KernelValue() noexcept { return ispc::Twice(21); }\n");
        Write(fixture, "game/main.cpp", "int KernelValue() noexcept;\nint main() { return KernelValue() == 42 ? 0 : 9; }\n");
        return template with
        {
            RootDirectory = fixture,
            OutputDirectory = output,
            Configuration = NativeConfiguration.Debug,
            Targets = ["Game"],
            Modules =
            [
                new()
                {
                    Name = "Kernel", Sources = [Path.Combine(fixture, "kernel", "user.cpp")], IspcSources = [Path.Combine(fixture, "kernel", "k.ispc")]
                },
                new() { Name = "Game", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(fixture, "game", "main.cpp")], PrivateDependencies = ["Kernel"] }
            ]
        };
    }

    private static void FixedArguments()
    {
        string[] windows = ["-O2", "--target=sse4-i32x4", "--arch=x86-64", "--target-os=windows", "--opt=disable-fma", "--math-lib=default"];
        Check(IspcTools.Arguments(NativePlatform.Windows, NativeArchitecture.X64).SequenceEqual(windows),
            "Windows X64 ISPC arguments: " + string.Join(' ', IspcTools.Arguments(NativePlatform.Windows, NativeArchitecture.X64)));
        string[] mac = ["-O2", "--target=neon-i32x4", "--arch=aarch64", "--target-os=macos", "--opt=disable-fma", "--math-lib=default", "--pic"];
        Check(IspcTools.Arguments(NativePlatform.MacOS, NativeArchitecture.Arm64).SequenceEqual(mac),
            "macOS Arm64 ISPC arguments: " + string.Join(' ', IspcTools.Arguments(NativePlatform.MacOS, NativeArchitecture.Arm64)));
        foreach (NativePlatform platform in new[] { NativePlatform.Windows, NativePlatform.MacOS })
        {
            foreach (NativeArchitecture architecture in new[] { NativeArchitecture.X64, NativeArchitecture.Arm64 })
            {
                IReadOnlyList<string> arguments = IspcTools.Arguments(platform, architecture);
                Check(arguments.Count(argument => argument.StartsWith("--target=", StringComparison.Ordinal)) == 1 &&
                    !arguments.Any(argument => argument.Contains(',', StringComparison.Ordinal) || argument.Contains("fast-math", StringComparison.Ordinal)),
                    $"{platform} {architecture}: one ISPC target and no fast-math");
            }
        }

        try
        {
            IspcTools.Arguments(NativePlatform.Android, NativeArchitecture.Arm64);
            throw new InvalidOperationException("Android ISPC arguments were produced");
        }
        catch (NotSupportedException)
        {
        }

        string identity = IspcTools.Identity(NativePlatform.Windows, NativeArchitecture.X64);
        Check(identity == "ispc " + ToolchainTable.IspcVersion + " " + string.Join(' ', windows), "ISPC identity: " + identity);
        Check(IspcTools.IdentityDefine(NativePlatform.Windows, NativeArchitecture.X64) == "ROCKS_ISPC_IDENTITY=\"" + identity + "\"",
            "ISPC identity define: " + IspcTools.IdentityDefine(NativePlatform.Windows, NativeArchitecture.X64));
    }

    private static void PlanShape(NativeBuildRequest kernel, string fixture)
    {
        var fake = new IspcTool(Path.Combine(fixture, "fake", "ispc.exe"), ToolchainTable.IspcVersion, new string('a', 64));
        NativePlan plan = ActionGraph.Create(kernel with { Ispc = fake });
        string output = kernel.OutputDirectory;
        string source = Path.Combine(fixture, "kernel", "k.ispc");
        string headers = IspcTools.HeaderDirectory(output, "Kernel");
        NativeAction ispc = plan.Actions.Single(action => action.Kind == "Ispc");
        string[] expected =
        [
            .. IspcTools.Arguments(kernel.Toolchain.Platform, kernel.Toolchain.Architecture), "--werror", "-I", Path.Combine(fixture, "kernel"), source,
            "-o", "{OUTPUT0}", "-h", "{OUTPUT1}", "-MMM", NativeAction.DepfilePlaceholder
        ];
        Check(ispc.Arguments.SequenceEqual(expected), "Ispc arguments:\n" + string.Join('\n', ispc.Arguments));
        Check(ispc.Tool == fake.Path && ispc.Module == "Kernel" && ispc.StageWorkingDirectory && ispc.DependencyFormat == DependencyFileFormat.Lines,
            "Ispc action runs the found tool in the stage directory with the line dependency format");
        Check(ispc.Outputs.Count == 3 && ispc.Outputs[1] == Path.Combine(headers, "k_ispc.h") && ispc.DependencyRecord == ispc.Outputs[2] &&
            ispc.Inputs.SequenceEqual([source]) && ispc.HeaderDirectories.SequenceEqual([Path.Combine(fixture, "kernel")]),
            "Ispc outputs (object, header, record), inputs and header directories");
        Check(ispc.Identity.SequenceEqual(["ispc", fake.Version, fake.Digest]), "Ispc identity: " + string.Join(' ', ispc.Identity));
        Check(Directory.Exists(headers), "the generated header directory is created at plan time");
        NativeAction archive = plan.Actions.Single(action => action.Kind == "Archive" && action.Module == "Kernel");
        Check(archive.Inputs.Contains(ispc.Outputs[0]) && archive.Dependencies.Contains(ispc.Id), "the ISPC object goes into the module archive");
        NativeAction compile = plan.Actions.Single(action => action.Kind == "Compile" && action.Module == "Kernel");
        bool windows = kernel.Toolchain.Platform == NativePlatform.Windows;
        Check(compile.Dependencies.Contains(ispc.Id) && compile.DeclaredInputs.Contains(ispc.Outputs[1]),
            "the C++ compile runs after Ispc and takes the generated header as a declared input");
        Check(compile.Arguments.Contains((windows ? "/I" : "-I") + headers) && !compile.HeaderDirectories.Contains(headers),
            "the generated header directory is an include directory but not a header directory");
        NativeAction game = plan.Actions.Single(action => action.Kind == "Compile" && action.Module == "Game");
        Check(!game.Arguments.Any(argument => argument.Contains(headers, StringComparison.Ordinal)) && !game.Dependencies.Contains(ispc.Id),
            "the generated headers are private to the module");
        Check(plan.LogicalCompiles.Single(compile => compile.Module == "Kernel").Arguments.Contains((windows ? "/I" : "-I") + headers),
            "the logical compile (compile database) sees the generated header directory");

        // ISPC 원본만 있는 서드파티 모듈도 archive를 만들고, 서드파티 ISPC 경고는 끈다(--woff).
        NativePlan only = ActionGraph.Create(kernel with
        {
            Ispc = fake,
            Modules = [kernel.Modules[0] with { Sources = [], ThirdParty = true }, kernel.Modules[1]]
        });
        Check(only.Actions.Any(action => action.Kind == "Archive" && action.Module == "Kernel"), "an ISPC-only module has an archive");
        NativeAction thirdParty = only.Actions.Single(action => action.Kind == "Ispc");
        Check(thirdParty.Arguments.Contains("--woff") && !thirdParty.Arguments.Contains("--werror"), "third-party ISPC warnings are off");

        // IDE 계획은 Ispc action 없이 include 디렉터리만 보인다(ISPC가 없어도 된다).
        NativePlan ide = ActionGraph.Create(kernel with { PlanMode = NativePlanMode.Ide, OutputDirectory = Path.Combine(fixture, "ide-out") });
        Check(!ide.Actions.Any(action => action.Kind == "Ispc") && !Directory.Exists(IspcTools.HeaderDirectory(Path.Combine(fixture, "ide-out"), "Kernel")),
            "the IDE plan has no Ispc action and writes nothing");
    }

    private static void Validation(NativeBuildRequest kernel, string fixture)
    {
        void Rejected(NativeBuildRequest request, string fragment, string name)
        {
            try
            {
                InputValidation.Validate(request);
                ActionGraph.Create(request);
                throw new InvalidOperationException(name + " was accepted");
            }
            catch (ArgumentException error) when (error.Message.Contains(fragment, StringComparison.Ordinal))
            {
            }
        }

        var fake = new IspcTool(Path.Combine(fixture, "fake", "ispc.exe"), ToolchainTable.IspcVersion, new string('a', 64));
        Write(fixture, "other/k.ispc", "export void Other() {}\n");
        NativeModuleInput module = kernel.Modules[0];
        Rejected(kernel with { Ispc = fake, Modules = [module with { IspcSources = [.. module.IspcSources, Path.Combine(fixture, "other", "k.ispc")] }, kernel.Modules[1]] },
            "have the same file name", "two ISPC sources with the same file name");
        Rejected(kernel with { Ispc = fake, Modules = [module with { IspcSources = [Path.Combine(fixture, "kernel", "user.cpp")] }, kernel.Modules[1]] },
            "must be the path of a .ispc source", "a .cpp in IspcSources");
        Rejected(kernel with { Ispc = fake, Toolchain = kernel.Toolchain with { Platform = NativePlatform.Android } },
            "not supported for Android", "ISPC sources for Android");
        Rejected(kernel, "has ISPC sources, but the request has no ISPC compiler", "a plan without an ISPC compiler");
        Rejected(kernel with { Ispc = fake, Modules = [module with { Sources = [] }, kernel.Modules[1]] },
            "needs C++ sources too", "an engine module with ISPC sources only");
        // Windows ISPC는 비 ASCII 경로를 열지 못한다. 시험 루트 위의 비 ASCII 조상(임시 폴더 이름)은 stage 상대 경로에 없으므로 받고,
        // 루트 아래의 비 ASCII 폴더는 계획 때 거부한다.
        if (kernel.Toolchain.Platform == NativePlatform.Windows)
        {
            Write(fixture, "커널/k.ispc", "export void Wide() {}\n");
            Rejected(kernel with { Ispc = fake, Modules = [module with { IspcSources = [Path.Combine(fixture, "커널", "k.ispc")] }, kernel.Modules[1]] },
                "cannot open the non-ASCII path", "a non-ASCII ISPC source folder on Windows");
        }
    }

    private static void DependencyParser(string fixture)
    {
        string cwd = Path.Combine(fixture, "stage");
        IReadOnlyList<string> parsed = IspcTools.ParseDependencies("../a b/k.ispc\r\n../a b\\inc.isph\n\n../a b/k.ispc\n" + Path.Combine(fixture, "abs.isph") + "\n", cwd);
        Check(parsed.SequenceEqual([Path.GetFullPath(Path.Combine(fixture, "a b", "k.ispc")), Path.GetFullPath(Path.Combine(fixture, "a b", "inc.isph")),
            Path.Combine(fixture, "abs.isph")]), "ISPC dependency lines: " + string.Join(" | ", parsed));
        Check(IspcTools.ParseDependencies("", cwd).Count == 0, "an empty ISPC dependency file has no dependencies");
        try
        {
            IspcTools.ParseDependencies("a\0b\n", cwd);
            throw new InvalidOperationException("a NUL in the ISPC dependency file was accepted");
        }
        catch (InvalidDataException)
        {
        }

        string version = ToolchainTable.IspcVersion;
        Check(IspcTools.IsVersion($"Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), {version} (build commit 1 @ 20260101, LLVM 20.1.8)", version) &&
            IspcTools.IsVersion($"Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), {version}", version) &&
            !IspcTools.IsVersion($"Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), {version}1", version) &&
            !IspcTools.IsVersion("Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), 1.20.0", version), "ISPC version line check");
    }

    private static async Task Discovery(NativeBuildRequest request, NativeBuildRequest kernel, string fixture)
    {
        Dictionary<string, string?> With(string value) => new(request.Toolchain.Environment) { [IspcTools.EnvironmentVariable] = value };
        string missing = Path.Combine(fixture, "missing", "ispc.exe");
        try
        {
            await IspcTools.DiscoverAsync(fixture, With(missing));
            throw new InvalidOperationException("a missing ROCKS_ISPC was accepted");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("points to a missing file", StringComparison.Ordinal) &&
            error.Message.Contains("InstallIspc.ps1", StringComparison.Ordinal))
        {
        }

        try
        {
            await IspcTools.DiscoverAsync(fixture, With(request.Toolchain.CompilerPath));
            throw new InvalidOperationException("a compiler that is not ISPC was accepted");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("but the build needs ISPC " + ToolchainTable.IspcVersion, StringComparison.Ordinal))
        {
        }

        // ISPC 원본이 빌드 대상에 없으면 ISPC를 찾지 않는다: ROCKS_ISPC가 없는 파일을 가리켜도 빌드가 된다.
        var builder = new NativeBuildEngine();
        NativeToolchain broken = request.Toolchain with { Environment = With(missing) };
        NativeBuildResult plain = await builder.BuildAsync(request with { Toolchain = broken, OutputDirectory = Path.Combine(fixture, "plain-out") });
        Check(plain.Succeeded, "a build without ISPC sources looked for ISPC: " + plain.Failure);
        Write(fixture, "plain/plain.cpp", "int main() { return 0; }\n");
        NativeBuildResult unreached = await builder.BuildAsync(kernel with
        {
            Toolchain = broken,
            OutputDirectory = Path.Combine(fixture, "unreached-out"),
            Modules = [.. kernel.Modules, new() { Name = "Plain", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(fixture, "plain", "plain.cpp")] }],
            Targets = ["Plain"]
        });
        Check(unreached.Succeeded && !unreached.Actions.Any(action => action.Kind == "Ispc"),
            "an ISPC module outside the build targets made the build look for ISPC: " + unreached.Failure);
        try
        {
            await builder.BuildAsync(kernel with { Toolchain = broken, OutputDirectory = Path.Combine(fixture, "broken-out") });
            throw new InvalidOperationException("a build with ISPC sources and a missing ISPC was accepted");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("points to a missing file", StringComparison.Ordinal))
        {
        }
    }

    private static async Task RealBuild(NativeBuildRequest kernel, string fixture, IspcTool tool)
    {
        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Build(NativeBuildRequest request)
        {
            NativeBuildResult result = await builder.BuildAsync(request);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(result.Failure + "\n" + string.Join("\n", result.Actions.Select(action => action.StdOut + action.StdErr)));
            }

            return result;
        }

        bool Ran(NativeBuildResult result, string kind, string module) =>
            result.Actions.Any(action => action.Kind == kind && action.Module == module && action.Executed);

        NativeBuildResult first = await Build(kernel);
        Check(Ran(first, "Ispc", "Kernel") && Ran(first, "Compile", "Kernel"), "the first build compiles the ISPC and C++ sources");
        ProcessResult run = await ProcessRunner.RunAsync(new(first.ModuleOutputs["Game"], [], fixture, kernel.Toolchain.Environment));
        Check(run.ExitCode == 0, $"the executable with the ISPC kernel returned {run.ExitCode}: {run.StdErr}");
        Check((await Build(kernel)).ExecutedActions == 0, "an unchanged ISPC build ran actions");

        string header = Path.Combine(IspcTools.HeaderDirectory(kernel.OutputDirectory, "Kernel"), "k_ispc.h");
        byte[] headerBytes = File.ReadAllBytes(header);
        string headerText = System.Text.Encoding.UTF8.GetString(headerBytes);
        Check(headerText.Contains("// k_ispc.h", StringComparison.Ordinal) && !headerText.Contains(".native-tmp", StringComparison.Ordinal) &&
            !headerText.Contains(fixture, StringComparison.OrdinalIgnoreCase), "the generated header names itself without a host path:\n" + headerText);

        // include한 .isph의 내용(의존 기록)이 바뀌면 Ispc만 다시 돈다 — 생성 헤더가 같으면 C++ compile은 그대로다.
        Write(fixture, "kernel/inc.isph", "// changed\nstatic inline uniform int Offset() { return 0; }\n");
        NativeBuildResult included = await Build(kernel);
        Check(Ran(included, "Ispc", "Kernel") && !Ran(included, "Compile", "Kernel"), "an included .isph change reruns Ispc only");

        // 원본 디렉터리에 새 .isph가 생기면(이름 목록) 다시 돈다.
        Write(fixture, "kernel/extra.isph", "// new include file\n");
        Check(Ran(await Build(kernel), "Ispc", "Kernel"), "a new .isph in the ISPC source directory reruns Ispc");

        // 버전 문자열이 다르면(같은 실행 파일이라도) 다시 돈다.
        Check(Ran(await Build(kernel with { Ispc = tool with { Version = tool.Version + "-other" } }), "Ispc", "Kernel"), "an ISPC version change reruns Ispc");
        Check(Ran(await Build(kernel), "Ispc", "Kernel"), "returning to the pinned ISPC version reruns Ispc");

        // 같은 버전 문자열의 다른 ISPC 빌드(digest)도 다시 돈다.
        Check(Ran(await Build(kernel with { Ispc = tool with { Digest = new string('0', 64) } }), "Ispc", "Kernel"), "an ISPC binary change reruns Ispc");
        Check(Ran(await Build(kernel), "Ispc", "Kernel"), "returning to the original ISPC binary reruns Ispc");

        // 원본이 바뀌어 내보내는 함수가 바뀌면 생성 헤더가 바뀌어 C++ compile도 다시 돈다.
        Write(fixture, "kernel/k.ispc", "#include \"inc.isph\"\nexport uniform int Twice(uniform int value) { return value * 2 + Offset(); }\n" +
            "export uniform int Thrice(uniform int value) { return value * 3; }\n");
        NativeBuildResult exported = await Build(kernel);
        Check(Ran(exported, "Ispc", "Kernel") && Ran(exported, "Compile", "Kernel"), "a changed generated header recompiles the C++ consumer");

        // 낡은 생성 헤더는 성공한 빌드가 지운다(이름을 바꾼 원본의 옛 헤더가 증분 빌드에서만 include되지 않게).
        string stale = Path.Combine(IspcTools.HeaderDirectory(kernel.OutputDirectory, "Kernel"), "old_ispc.h");
        File.WriteAllText(stale, "// stale\n");
        await Build(kernel);
        Check(!File.Exists(stale) && File.Exists(header), "a stale generated header survived a successful build");

        // 생성 헤더 결정성: 깨끗한 두 빌드(출력이 다르다)의 _ispc.h byte가 같다.
        string second = Path.Combine(fixture, "out-clean");
        await Build(kernel with { OutputDirectory = second });
        byte[] left = File.ReadAllBytes(Path.Combine(IspcTools.HeaderDirectory(kernel.OutputDirectory, "Kernel"), "k_ispc.h"));
        byte[] right = File.ReadAllBytes(Path.Combine(IspcTools.HeaderDirectory(second, "Kernel"), "k_ispc.h"));
        Check(left.SequenceEqual(right), "two clean builds wrote different generated headers");
    }

    private static void BuildIdInputs(NativeBuildRequest kernel, string fixture)
    {
        // BuildInfo Prepare처럼 계획의 생성 파일(정의 헤더)을 먼저 쓴다. ISPC 생성 헤더는 아직 없다(깨끗한 빌드의 Prepare와 같다).
        string Manifest(NativeBuildRequest request)
        {
            NativePlan plan = ActionGraph.Create(request);
            foreach (PlannedFile file in plan.GeneratedFiles)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(file.Path)!);
                File.WriteAllText(file.Path, file.Content);
            }

            return BuildInfoGenerator.Manifest(request, plan, "Game", new string('0', 64), new FileDigests());
        }

        string text = Manifest(kernel);
        Check(text.Contains("action: Ispc src:kernel/k.ispc\n", StringComparison.Ordinal) &&
            text.Contains("arg: ispc-version=" + ToolchainTable.IspcVersion + "\n", StringComparison.Ordinal) &&
            text.Contains("obj:src:kernel/k.ispc", StringComparison.Ordinal) && text.Contains("input: src:kernel/k.ispc ", StringComparison.Ordinal) &&
            text.Contains("input: src:kernel/inc.isph ", StringComparison.Ordinal) &&
            !text.Contains(Path.GetDirectoryName(kernel.Ispc!.Path)!, StringComparison.OrdinalIgnoreCase), "ISPC lines of the tool build ID manifest:\n" + text);

        // 다른 루트(같은 내용)에서도 같은 텍스트다.
        string moved = Path.Combine(Path.GetDirectoryName(fixture)!, "ispc moved fixture");
        foreach (string file in new[] { "kernel/k.ispc", "kernel/inc.isph", "kernel/extra.isph", "kernel/user.cpp", "game/main.cpp" })
        {
            Write(moved, file, File.ReadAllText(Path.Combine(fixture, file)));
        }

        NativeBuildRequest relocated = Fixture(kernel, moved, Path.Combine(moved, "out")) with { Ispc = kernel.Ispc, EngineVersion = kernel.EngineVersion };
        foreach (string file in new[] { "kernel/k.ispc", "kernel/inc.isph" })
        {
            Write(moved, file, File.ReadAllText(Path.Combine(fixture, file)));
        }

        Check(Manifest(relocated) == text, "the tool build ID manifest depends on the host root:\n" + Manifest(relocated));
        Write(moved, "kernel/k.ispc", File.ReadAllText(Path.Combine(fixture, "kernel/k.ispc")) + "// changed\n");
        Check(Manifest(relocated) != text, "an ISPC source change does not change the tool build ID manifest");
    }

    private static void Write(string root, string relative, string text)
    {
        string path = Path.Combine(root, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
