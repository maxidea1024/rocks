using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 빌드 구성(Debug·Development·Release·Asan)과 모듈 최적화 재정의가 만드는 compile·link 인자를 검사하고,
/// 실제 toolchain으로 Development와 Release(ThinLTO, 최적화를 끈 모듈과 섞인 경우)를 빌드·실행한다.
/// </summary>
internal static class BuildConfigurationTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        string Optimized() => windows ? "/O2" : "-O2";
        string Unoptimized() => windows ? "/Od" : "-O0";
        string DebugInfo() => windows ? "/Z7" : "-g";
        string Define(string value) => (windows ? "/D" : "-D") + value;

        IReadOnlyList<string> CompileArguments(NativeBuildRequest input, string module) =>
            ActionGraph.Create(input).Actions.Single(action => action.Kind == "Compile" && action.Module == module).Arguments;

        void Expect(NativeConfiguration configuration, string optimization, bool ndebug, bool lto, string buildMacro)
        {
            var arguments = CompileArguments(request with { Configuration = configuration }, "Engine");
            string name = configuration.ToString();
            Check(arguments.Contains(optimization), $"{name}: expected {optimization}");
            Check(arguments.Contains(Define(buildMacro)), $"{name}: expected {buildMacro}");
            Check(arguments.Count(argument => argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) == 1,
                $"{name}: exactly one build configuration macro");
            Check(arguments.Contains(Define("NDEBUG")) == ndebug, $"{name}: NDEBUG mismatch");
            // 디버그 정보는 모든 구성에서 만든다(Release도 크래시 덤프 심볼화를 위해 PDB를 남긴다).
            Check(arguments.Contains(DebugInfo()), $"{name}: debug information missing");
            Check(arguments.Contains("-flto=thin") == lto, $"{name}: ThinLTO mismatch");
            if (windows)
            {
                Check(arguments.Contains(configuration == NativeConfiguration.Debug ? "/MDd" : "/MD"), $"{name}: CRT mismatch");
            }
        }

        Expect(NativeConfiguration.Debug, Unoptimized(), ndebug: false, lto: false, "ROCKS_BUILD_DEBUG=1");
        Expect(NativeConfiguration.Development, Optimized(), ndebug: false, lto: false, "ROCKS_BUILD_DEVELOPMENT=1");
        Expect(NativeConfiguration.Release, Optimized(), ndebug: true, lto: true, "ROCKS_BUILD_RELEASE=1");
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            Expect(NativeConfiguration.Asan, Unoptimized(), ndebug: false, lto: false, "ROCKS_BUILD_DEBUG=1");
        }

        // 모든 구성의 실행 파일이 PDB를 남긴다(Windows).
        if (windows)
        {
            foreach (NativeConfiguration configuration in Enum.GetValues<NativeConfiguration>())
            {
                var link = ActionGraph.Create(request with { Configuration = configuration }).Actions.Single(action => action.Kind == "Link");
                Check(link.Outputs.Any(path => path.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase)) &&
                      link.Arguments.Contains("/DEBUG:FULL"), configuration + " link must produce a PDB");
            }
        }
        else
        {
            var link = ActionGraph.Create(request with { Configuration = NativeConfiguration.Release })
                .Actions.Single(action => action.Kind == "Link");
            Check(link.Arguments.Contains("-flto=thin"), "POSIX Release link driver must enable ThinLTO");
        }

        // 모듈 재정의: Never는 Release에서도 최적화·ThinLTO를 끄고, Always는 Debug에서도 최적화한다.
        // CRT·구성 매크로·NDEBUG는 구성을 따른다.
        NativeBuildRequest WithEngine(NativeConfiguration configuration, CodeOptimization optimization) => request with
        {
            Configuration = configuration,
            Modules = request.Modules.Select(module => module.Name == "Engine" ? module with { Optimization = optimization } : module).ToArray()
        };
        var never = CompileArguments(WithEngine(NativeConfiguration.Release, CodeOptimization.Never), "Engine");
        Check(never.Contains(Unoptimized()) && !never.Contains(Optimized()) && !never.Contains("-flto=thin") &&
              never.Contains(Define("NDEBUG")) && never.Contains(Define("ROCKS_BUILD_RELEASE=1")), "Never override in Release");
        var neighbour = CompileArguments(WithEngine(NativeConfiguration.Release, CodeOptimization.Never), "Game");
        Check(neighbour.Contains(Optimized()) && neighbour.Contains("-flto=thin"), "Never override leaked to another module");
        var always = CompileArguments(WithEngine(NativeConfiguration.Debug, CodeOptimization.Always), "Engine");
        Check(always.Contains(Optimized()) && !always.Contains(Unoptimized()) && !always.Contains("-flto=thin") &&
              (!windows || always.Contains("/MDd")) && always.Contains(Define("ROCKS_BUILD_DEBUG=1")), "Always override in Debug");
        // 재정의가 바뀌면 compile action identity가 바뀌어 다시 빌드된다.
        var defaultIdentity = ActionGraph.Create(WithEngine(NativeConfiguration.Release, CodeOptimization.Default)).Actions
            .Single(action => action.Kind == "Compile" && action.Module == "Engine").Identity;
        var neverIdentity = ActionGraph.Create(WithEngine(NativeConfiguration.Release, CodeOptimization.Never)).Actions
            .Single(action => action.Kind == "Compile" && action.Module == "Engine").Identity;
        Check(!defaultIdentity.SequenceEqual(neverIdentity), "Optimization override must change the compile identity");

        // 잘못된 입력: 구성 매크로 정의, 최적화·LTO 인자 직접 지정, 정의되지 않은 enum 값.
        var builder = new NativeBuildEngine();
        async Task Rejects(NativeBuildRequest input, string message, string? diagnostic = null)
        {
            try
            {
                await builder.BuildAsync(input with { OutputDirectory = Path.Combine(root, "configuration-invalid") });
            }
            catch (ArgumentException error)
            {
                if (diagnostic != null && !error.Message.Contains(diagnostic, StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(message + ": unexpected diagnostic " + error.Message);
                }

                return;
            }

            throw new InvalidOperationException(message);
        }

        NativeBuildRequest WithEngineModule(Func<NativeModuleInput, NativeModuleInput> change) => request with
        {
            Modules = request.Modules.Select(module => module.Name == "Engine" ? change(module) : module).ToArray()
        };
        foreach (string define in new[] { "ROCKS_BUILD_RELEASE=1", "ROCKS_PLATFORM_WINDOWS=1", "ROCKS_ARCH_ARM64=1" })
        {
            await Rejects(WithEngineModule(module => module with { PrivateDefines = [define] }), "module defined a reserved macro " + define,
                "reserved for the build configuration");
        }

        foreach (string argument in new[] { "-O", "-O3", "-Os", "-Ofast", "--optimize=2", "/O1", "/O2", "/Ox", "/Od", "/Ob2",
                     "-flto", "-flto=full", "-fno-lto" })
        {
            await Rejects(WithEngineModule(module => module with { AdditionalCompileArguments = [argument] }), "accepted " + argument,
                "overrides owned output/policy");
        }

        foreach (string argument in new[] { "-flto=thin", "-fno-lto", "/opt:lldlto=0", "-Wl,--lto-O0", "-Wl,--thinlto-jobs=1" })
        {
            await Rejects(WithEngineModule(module => module with { AdditionalLinkArguments = [argument] }), "accepted link " + argument,
                "overrides owned output/policy");
        }
        // 최적화 수준을 바꾸지 않는 인자는 받는다(frame pointer 유지 등).
        foreach (string argument in windows ? new[] { "/Oy-", "/Oi" } : ["-fno-omit-frame-pointer"])
        {
            InputValidation.Validate(WithEngineModule(module => module with { AdditionalCompileArguments = [argument] }));
        }

        await Rejects(WithEngineModule(module => module with { Optimization = (CodeOptimization)7 }), "accepted undefined optimization",
            "unsupported Optimization");
        await Rejects(request with { Configuration = (NativeConfiguration)9 }, "accepted undefined configuration");
        if (windows)
        {
            // MSVC lib.exe·link.exe는 ThinLTO bitcode를 읽지 못하므로 Release를 거부한다. 실제로 존재하는
            // 가짜 파일을 써서 "경로 없음"이 아니라 도구 종류 때문에 거부되는지 확인한다.
            string msvc = Directory.CreateDirectory(Path.Combine(root, "msvc-tools")).FullName;
            string lib = Path.Combine(msvc, "lib.exe");
            string link = Path.Combine(msvc, "link.exe");
            File.WriteAllText(lib, "");
            File.WriteAllText(link, "");
            await Rejects(request with { Configuration = NativeConfiguration.Release, Toolchain = request.Toolchain with { ArchiverPath = lib } },
                "Release accepted MSVC lib.exe", "ThinLTO");
            await Rejects(request with { Configuration = NativeConfiguration.Release, Toolchain = request.Toolchain with { LinkerPath = link } },
                "Release accepted MSVC link.exe", "ThinLTO");
            // 이름이 맞아도 compiler와 다른 LLVM 설치(다른 폴더)의 도구는 bitcode 버전이 다를 수 있어 거부한다.
            string otherLlvm = Path.Combine(msvc, "llvm-lib.exe");
            File.WriteAllText(otherLlvm, "");
            await Rejects(request with { Configuration = NativeConfiguration.Release, Toolchain = request.Toolchain with { ArchiverPath = otherLlvm } },
                "Release accepted llvm-lib from another LLVM", "compiler's directory");
            // 실제 toolchain 탐색은 compiler 옆의 도구를 고른다.
            string compilerDirectory = Path.GetDirectoryName(request.Toolchain.CompilerPath)!;
            Check(string.Equals(Path.GetDirectoryName(request.Toolchain.ArchiverPath), compilerDirectory, StringComparison.OrdinalIgnoreCase) &&
                  string.Equals(Path.GetDirectoryName(request.Toolchain.LinkerPath), compilerDirectory, StringComparison.OrdinalIgnoreCase),
                "toolchain discovery must prefer llvm-lib and lld-link beside the compiler");
        }

        // 실제 빌드·실행: Development, 그리고 ThinLTO 모듈과 최적화를 끈 모듈이 섞인 Release.
        foreach (var (name, input) in new[]
                 {
                     ("development", request with { Configuration = NativeConfiguration.Development }),
                     ("release-mixed", WithEngine(NativeConfiguration.Release, CodeOptimization.Never)),
                     ("debug-always", WithEngine(NativeConfiguration.Debug, CodeOptimization.Always))
                 })
        {
            var result = await builder.BuildAsync(input with { OutputDirectory = Path.Combine(root, "configuration-" + name) });
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(name + ": " + result.Failure + "\n" +
                    string.Join("\n", result.Actions.Select(action => action.StdOut + action.StdErr)));
            }

            if (request.Toolchain.Platform == NativePlatform.Android)
            {
                continue;
            }

            var run = await ProcessRunner.RunAsync(new(result.ModuleOutputs["Game"], [], root, request.Toolchain.Environment));
            Check(run.ExitCode == 0, name + " executable failed: " + run.StdErr);
        }

        // release-mixed: ThinLTO 모듈(Game)은 LLVM bitcode object, 최적화를 끈 모듈(Engine)은 native object다.
        // bitcode는 raw("BC" C0 DE)이거나, Apple clang처럼 Darwin bitcode wrapper(magic 0x0B17C0DE, little-endian
        // 헤더의 offset 위치에 raw bitcode)로 감싸져 있다.
        static bool IsRawBitcode(byte[] bytes, int offset) => offset >= 0 && offset + 4 <= bytes.Length &&
            bytes[offset] == 0x42 && bytes[offset + 1] == 0x43 && bytes[offset + 2] == 0xc0 && bytes[offset + 3] == 0xde;
        static bool IsBitcode(string directory) => Directory.EnumerateFiles(directory).All(path =>
        {
            byte[] bytes = File.ReadAllBytes(path);
            if (IsRawBitcode(bytes, 0))
            {
                return true;
            }

            bool wrapper = bytes.Length >= 20 && bytes[0] == 0xde && bytes[1] == 0xc0 && bytes[2] == 0x17 && bytes[3] == 0x0b;
            return wrapper && IsRawBitcode(bytes, (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(8, 4)));
        });
        string mixedObjects = Path.Combine(root, "configuration-release-mixed", "obj");
        Check(IsBitcode(Path.Combine(mixedObjects, "Game")), "Release ThinLTO module must compile to LLVM bitcode");
        Check(!IsBitcode(Path.Combine(mixedObjects, "Engine")), "Never-optimized module must stay a native object");

        // 같은 출력 폴더에서 Default → Never로 바꾸면 그 모듈 compile과 실행 파일 link가 다시 실행되고, 다른
        // 모듈 compile은 재사용한다. 같은 설정으로 다시 빌드하면 아무것도 실행하지 않는다.
        string incremental = Path.Combine(root, "configuration-incremental");
        async Task<NativeBuildResult> BuildIncremental(CodeOptimization optimization)
        {
            var result = await builder.BuildAsync(WithEngine(NativeConfiguration.Release, optimization) with { OutputDirectory = incremental });
            Check(result.Succeeded, "incremental optimization build failed: " + result.Failure);
            return result;
        }

        await BuildIncremental(CodeOptimization.Default);
        var switched = await BuildIncremental(CodeOptimization.Never);
        Check(switched.Actions.Any(action => action.Module == "Engine" && action.Kind == "Compile" && action.Executed) &&
              switched.Actions.Any(action => action.Kind == "Link" && action.Executed) &&
              !switched.Actions.Any(action => action.Module == "Game" && action.Kind == "Compile" && action.Executed),
            "optimization override change must rebuild only the module and relink");
        Check((await BuildIncremental(CodeOptimization.Never)).ExecutedActions == 0, "unchanged optimization override rebuilt");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
