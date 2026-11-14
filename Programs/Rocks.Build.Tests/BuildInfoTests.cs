using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 빌드 주입 상수(Docs/Design/ReflectionFoundations.ko.md 8.1절)를 검사한다: 툴체인 지문 정규 텍스트와 조합 동일, 툴체인 신원
/// discovery, 빌드 환경 위생, FP 모델 인자와 모듈 인자 거부, 실제 빌드의 주입(가짜 Core ← Engine ← Game), 증분·줄 끝·호스트 무관,
/// 구성 사이 지문 동일, 외부 경로 키, include inventory 제외, 실패한 link.
/// </summary>
internal static class BuildInfoTests
{
    private static readonly string LowerA = new('a', 64);

    private static readonly string LowerB = new('b', 64);

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        NativeToolchain toolchain = request.Toolchain;
        bool windows = toolchain.Platform == NativePlatform.Windows;

        FingerprintCanonicalText();
        pass("BuildInfo: fingerprint canonical text golden, MSVC suffix removal, identity changes");

        CompilerVersionAndTriple(toolchain);
        pass("BuildInfo: version line brackets, effective triple checks, real discovery identity without host paths");

        await EnvironmentHygieneAsync(request, root);
        pass("BuildInfo: allowlisted build environment drops and reports CL/LINK/COMPILER_PATH, session variables stay out of the key");

        ModelArgumentRejection(request, windows);
        pass("BuildInfo: FP model, LLVM, linker, CL dash-form and absolute-path module arguments rejected");

        FileDigestRules(root);
        pass("BuildInfo: FileDigests line-ending normalization by extension, buffer-boundary CR carry");

        await InjectionAndIncrementalAsync(request, root);
        pass("BuildInfo: injected digests equal C# fingerprint/manifest, marker required, incremental and line endings");

        await HostIndependenceAsync(request, root);
        pass("BuildInfo: ID and generated TU are identical across roots, output names, subst, 8.3 and junction");

        await ConfigurationsAndFailuresAsync(request, root);
        pass("BuildInfo: configurations share the fingerprint, ext keys, inventory exclusions, failed link, empty identity");

        await PathAndRootRulesAsync(request, root);
        pass("BuildInfo: host path boundaries, single percent-encoding, owned roots inside sources, absolute roots, argument layout");
    }

    // 9. 경로 규칙: 남은 호스트 경로 검사의 경계, percent-encoding(C0·C1·U+2028/2029·%·NFC, 한 번만), src 안의 소유 루트, 절대 루트 요구,
    // 소스 없는 실행 파일, 툴체인 인자 부분열 불일치.
    private static async Task PathAndRootRulesAsync(NativeBuildRequest request, string root)
    {
        foreach ((string text, string hostRoot, bool windows, bool expected) in new[]
                 {
                     ("-I/srcx", "/src", false, false), ("/src/a.h", "/src", false, true), ("x/src/a", "/src", false, false),
                     ("-DA=/src", "/src", false, true), ("-I/src/a", "/src", false, true), ("/srcfoo", "/src", false, false),
                     ("/clang:-x", "/c", false, false), ("-fprofile=/c/x", "/c", false, true), ("a/c/b", "/c", false, false),
                     (@"c:\r\x", @"C:\R", true, true), ("C:/R/x", @"C:\R", true, true), (@"C:\Rx", @"C:\R", true, false),
                     (@"/IC:\R\inc", @"C:\R", true, true), ("-DX=C:/R", @"C:\R", true, true), ("D:/RC:/R", @"C:\R", true, false),
                     (@"R:\x", @"R:\", true, true), ("/INCLUDE:__asan_seh_interceptor", @"R:\", true, false), ("-DX=r:/y", @"R:\", true, true)
                 })
        {
            Check(BuildInfoGenerator.ContainsHostPath(text, hostRoot, windows) == expected,
                $"host path boundary: '{text}' with root '{hostRoot}' expected {expected}");
        }

        string controls = "a" + (char)1 + "b%c" + (char)0x85 + (char)0x2028 + (char)0x2029 + (char)0x7f + "d";
        Check(PortablePaths.Encode(controls) == "a%01b%25c%C2%85%E2%80%A8%E2%80%A9%7Fd", "percent-encoding: " + PortablePaths.Encode(controls));
        Check(PortablePaths.Encode("e" + (char)0x301 + ".h") == (char)0xe9 + ".h", "path keys must be NFC");
        string fixture = Directory.CreateDirectory(Path.Combine(root, "buildinfo-paths")).FullName;
        string output = Path.Combine(fixture, "out");
        var paths = new PortablePaths(fixture, output, null,
            new Dictionary<string, string> { ["resource"] = Path.Combine(output, "toolchains", "llvm") });
        Check(paths.Key(Path.Combine(output, "toolchains", "llvm", "lib", "a.lib")) == "toolchain:resource/lib/a.lib" &&
            paths.IsOwned(Path.Combine(output, "toolchains", "llvm", "lib", "a.lib")) && paths.Key(Path.Combine(output, "lib", "b.lib")) == "out:lib/b.lib" &&
            paths.Key(Path.Combine(fixture, "x%y.h")) == "src:x%25y.h", "owned roots inside out must win as the longest root");
        try
        {
            _ = new PortablePaths(fixture, output, null, new Dictionary<string, string> { ["resource"] = Path.GetDirectoryName(fixture)! });
            throw new InvalidOperationException("an owned root containing the source root was accepted");
        }
        catch (ArgumentException error) when (error.Message.Contains("OwnedRoots", StringComparison.Ordinal) &&
            error.Message.Contains("contains the", StringComparison.Ordinal))
        {
        }

        if (!OperatingSystem.IsWindows())
        {
            Check(NativeToolchains.IsXcrunShim("/usr/bin/clang++") && !NativeToolchains.IsXcrunShim("/opt/homebrew/opt/llvm/bin/clang++"),
                "xcrun shim detection");
        }

        WriteFixture(fixture);
        NativeBuildRequest game = FixtureRequest(request, fixture, output);
        var builder = new NativeBuildEngine();
        foreach ((string name, NativeBuildRequest relative) in new[]
                 {
                     ("RootDirectory", game with { RootDirectory = "relative-root" }),
                     ("OutputDirectory", game with { OutputDirectory = "relative-out" }),
                     ("SdkDirectory", game with { SdkDirectory = "relative-sdk" })
                 })
        {
            try
            {
                await builder.BuildAsync(relative);
                throw new InvalidOperationException("relative " + name + " accepted");
            }
            catch (ArgumentException error) when (error.Message.Contains(name + " must be an absolute path", StringComparison.Ordinal))
            {
            }
        }

        // 소스 없는 실행 파일은 BuildInfo 대상이 되기 전에 거부된다(생성 TU만으로 main 없는 link를 시도하지 않는다).
        try
        {
            await builder.BuildAsync(game with
            {
                Modules = [.. game.Modules, new() { Name = "Empty", Kind = NativeModuleKind.Executable, Dependencies = ["Engine"] }],
                Targets = ["Empty"]
            });
            throw new InvalidOperationException("an executable without sources was accepted");
        }
        catch (ArgumentException error) when (error.Message.Contains("Executable requires a source", StringComparison.Ordinal))
        {
        }

        Check(!File.Exists(Path.Combine(output, "BuildInfo", "Empty.BuildInfo.cpp")), "an executable without sources received a BuildInfo TU");
        // ActionGraph가 툴체인 인자를 정한 자리에 두지 않으면(배치 변경) manifest를 만들지 않는다.
        NativePlan plan = ActionGraph.Create(game);
        NativeBuildRequest moved = game with
        {
            Toolchain = game.Toolchain with { CompileArguments = [.. game.Toolchain.CompileArguments, "--rocks-layout-probe"] }
        };
        try
        {
            BuildInfoGenerator.Manifest(moved, plan, "Game", new string('0', 64), new FileDigests());
            throw new InvalidOperationException("a manifest was built from mismatched toolchain arguments");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("does not carry the toolchain arguments", StringComparison.Ordinal))
        {
        }
    }

    // 1. 정규 텍스트 golden. Windows는 MSVC 환경 버전 접미를 지우고, macOS 배포 대상·Android API 수준은 남긴다.
    private static void FingerprintCanonicalText()
    {
        NativeToolchain manual = Manual(NativePlatform.Windows, "x86_64-pc-windows-msvc19.29.30159");
        string expected = "rocks-toolchain-fingerprint 1\n" +
            "compiler: clang version 21.1.6\n" +
            "compiler-sha256: " + LowerA + "\n" +
            "linker-sha256: " + LowerB + "\n" +
            "target: x86_64-pc-windows-msvc\n" +
            "fp: /clang:-ffp-contract=off\n";
        Check(ToolchainFingerprint.CanonicalText(manual) == expected, "Windows canonical text:\n" + ToolchainFingerprint.CanonicalText(manual));
        Check(ToolchainFingerprint.Compute(manual).SequenceEqual(SHA256.HashData(Encoding.UTF8.GetBytes(expected))),
            "Compute must hash the UTF-8 canonical text");
        NativeToolchain mac = Manual(NativePlatform.MacOS, "arm64-apple-macosx13.0.0") with
        {
            CompileArguments = [NativeToolchains.NoDefaultConfig, "-mmacosx-version-min=13.0", "-isysroot", "/Applications/Xcode.app/SDK"]
        };
        Check(ToolchainFingerprint.CanonicalText(mac).EndsWith("target: arm64-apple-macosx13.0.0\nfp: -ffp-contract=off\n", StringComparison.Ordinal),
            "macOS canonical text keeps the deployment target and ignores -mmacosx-version-min:\n" + ToolchainFingerprint.CanonicalText(mac));
        NativeToolchain android = Manual(NativePlatform.Android, "aarch64-unknown-linux-android26");
        Check(ToolchainFingerprint.CanonicalText(android).Contains("target: aarch64-unknown-linux-android26\n", StringComparison.Ordinal),
            "Android canonical text keeps the API level");

        byte[] baseline = ToolchainFingerprint.Compute(manual);
        foreach ((string name, NativeToolchain changed) in new[]
                 {
                     ("version", manual with { CompilerVersion = "clang version 21.1.7" }),
                     ("triple", manual with { TargetTriple = "aarch64-pc-windows-msvc19.29.30159" }),
                     ("compiler digest", manual with { CompilerDigest = new string('c', 64) }),
                     ("linker digest", manual with { LinkerDigest = new string('d', 64) })
                 })
        {
            Check(!ToolchainFingerprint.Compute(changed).SequenceEqual(baseline), "fingerprint ignored the " + name);
        }

        foreach ((string name, NativeToolchain same) in new[]
                 {
                     ("MSVC environment version", manual with { TargetTriple = "x86_64-pc-windows-msvc19.44.35207" }),
                     ("host paths", manual with { CompilerPath = "D:/other/clang-cl.exe", Identity = "InstalledDir: D:/other" }),
                     ("environment", manual with { Environment = new Dictionary<string, string?> { ["PATH"] = "D:/other" } }),
                     ("ABI fields", manual with { AbiFields = ["VCToolsVersion=14.44.35207"] })
                 })
        {
            Check(ToolchainFingerprint.Compute(same).SequenceEqual(baseline), "fingerprint depends on " + name);
        }

        // 결정 7: 구성·최적화·언어 조합마다 FP 모델 인자가 다르면 지문을 만들지 않는다.
        try
        {
            ToolchainFingerprint.CanonicalText(manual, (configuration, _, _) =>
                configuration == NativeConfiguration.Release ? ["/clang:-ffp-contract=off", "-ffast-math"] : ["/clang:-ffp-contract=off"]);
            throw new InvalidOperationException("differing FP model arguments were accepted");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("differ between", StringComparison.Ordinal))
        {
        }

        Check(ToolchainFingerprint.PortableTarget("x86_64-pc-windows-msvc") == "x86_64-pc-windows-msvc" &&
            ToolchainFingerprint.PortableTarget("x86_64-pc-windows-msvc19.29.30159") == "x86_64-pc-windows-msvc" &&
            ToolchainFingerprint.PortableTarget("arm64-apple-macosx13.0.0") == "arm64-apple-macosx13.0.0", "MSVC suffix removal");
    }

    // 2. 버전 줄 괄호 규칙과 effective triple 검사, 실제 discovery 신원.
    private static void CompilerVersionAndTriple(NativeToolchain toolchain)
    {
        foreach ((string output, string expected) in new[]
                 {
                     ("clang version 21.1.6\nTarget: x86_64-pc-windows-msvc\nInstalledDir: C:/LLVM/bin\n", "clang version 21.1.6"),
                     ("clang version 21.1.6 (https://github.com/llvm/llvm-project a832a5222e489298337fbb5876f8dcaf072c5cca)\r\nTarget: x\r\n",
                         "clang version 21.1.6 (a832a5222e489298337fbb5876f8dcaf072c5cca)"),
                     ("Android (13065274, based on r547379) clang version 20.0.0 (https://android.googlesource.com/toolchain/llvm-project b718bcaf8c198c82f3021447d943401e3ab5bd54)\n",
                         "Android (13065274, based on r547379) clang version 20.0.0 (b718bcaf8c198c82f3021447d943401e3ab5bd54)"),
                     ("\n  clang version 22.0.0git (C:/src/llvm-project 1a2b3c4d)\n", "clang version 22.0.0git (1a2b3c4d)"),
                     ("clang version 22.0.0git (/home/user/llvm-project)\n", "clang version 22.0.0git"),
                     ("Apple clang version 16.0.0 (clang-1600.0.26.6)\nTarget: arm64-apple-darwin24.1.0\n", "Apple clang version 16.0.0 (clang-1600.0.26.6)")
                 })
        {
            string actual = NativeToolchains.ParseCompilerVersion(output);
            Check(actual == expected, $"compiler version '{actual}' expected '{expected}'");
        }

        foreach (string invalid in new[] { "", "gcc (GCC) 14.1.0\n", "\n\n" })
        {
            try
            {
                NativeToolchains.ParseCompilerVersion(invalid);
                throw new InvalidOperationException("non-clang version line accepted: " + invalid);
            }
            catch (InvalidOperationException error) when (error.Message.Contains("clang version", StringComparison.Ordinal))
            {
            }
        }

        Check(NativeToolchains.ParseTargetTriple("x86_64-pc-windows-msvc19.29.30159\r\n") == "x86_64-pc-windows-msvc19.29.30159",
            "effective triple with CRLF");
        foreach (string invalid in new[] { "", "\n", "a\nb\n", "x86 64-pc", "x86_64\n\n", "C:\\triple" })
        {
            try
            {
                NativeToolchains.ParseTargetTriple(invalid);
                throw new InvalidOperationException("invalid effective triple accepted: " + invalid);
            }
            catch (InvalidOperationException error) when (error.Message.Contains("effective target triple", StringComparison.Ordinal))
            {
            }
        }

        // 대상 아키텍처는 triple의 첫 조각으로 정한다. 32 bit·모르는 아키텍처와 빈 triple은 거부한다.
        foreach ((string triple, NativeArchitecture expected) in new[] {
            ("x86_64-pc-windows-msvc19.29.30159", NativeArchitecture.X64), ("amd64-unknown-freebsd", NativeArchitecture.X64),
            ("arm64-apple-macosx13.0.0", NativeArchitecture.Arm64), ("aarch64-unknown-linux-android26", NativeArchitecture.Arm64),
            ("arm64e-apple-ios17.0", NativeArchitecture.Arm64) })
        {
            Check(NativeArchitectures.FromTriple(triple) == expected, "architecture of " + triple);
        }

        foreach (string invalid in new[] { "", "i686-pc-windows-msvc", "armv7-none-linux-androideabi26", "riscv64-unknown-linux" })
        {
            try
            {
                NativeArchitectures.FromTriple(invalid);
                throw new InvalidOperationException("unsupported architecture accepted: " + invalid);
            }
            catch (ArgumentException)
            {
            }
        }

        // 실제 discovery: 신원 필드가 채워지고, 지문 텍스트에 컴파일러 디렉터리 경로가 없다.
        Check(toolchain.CompilerVersion.Contains("clang version", StringComparison.Ordinal), "discovered CompilerVersion: " + toolchain.CompilerVersion);
        Check(NativeToolchains.ParseTargetTriple(toolchain.TargetTriple) == toolchain.TargetTriple, "discovered TargetTriple: " + toolchain.TargetTriple);
        Check(toolchain.Architecture == (System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture ==
            System.Runtime.InteropServices.Architecture.Arm64 ? NativeArchitecture.Arm64 : NativeArchitecture.X64),
            "host toolchain architecture follows the host process: " + toolchain.TargetTriple);
        Check(toolchain.CompileArguments.FirstOrDefault() == NativeToolchains.NoDefaultConfig &&
            toolchain.LinkArguments.FirstOrDefault() == NativeToolchains.NoDefaultConfig,
            "discovery must put --no-default-config first in compile and link arguments");
        Check(toolchain.AbiFields.SequenceEqual(toolchain.AbiFields.Order(StringComparer.Ordinal)) &&
            toolchain.AbiFields.Contains("compiler=" + toolchain.CompilerVersion), "AbiFields must be sorted and name the compiler");
        Check(toolchain.AbiFields.All(field => field.IndexOfAny(['/', '\\']) < 0 || field.StartsWith("compiler=", StringComparison.Ordinal)),
            "AbiFields must not contain paths: " + string.Join(";", toolchain.AbiFields));
        Check(toolchain.OwnedRoots.ContainsKey("resource"), "discovery must name the resource directory as a toolchain-owned root");
        if (toolchain.Platform == NativePlatform.Windows)
        {
            Check(toolchain.CompilerDigest == Hashing.Sha256File(toolchain.CompilerPath), "CompilerDigest must hash the compiler binary");
            Check(toolchain.LinkerDigest == Hashing.Sha256File(toolchain.LinkerPath!), "LinkerDigest must hash LinkerPath");
        }

        string canonical = ToolchainFingerprint.CanonicalText(toolchain);
        string directory = Path.GetDirectoryName(toolchain.CompilerPath)!;
        foreach (string spelling in new[] { directory, directory.Replace('\\', '/') })
        {
            Check(!canonical.Contains(spelling, StringComparison.OrdinalIgnoreCase), "fingerprint text contains the compiler directory:\n" + canonical);
        }

        Check(!canonical.Contains("InstalledDir", StringComparison.Ordinal) && canonical.EndsWith('\n') && !canonical.Contains(" \n", StringComparison.Ordinal),
            "fingerprint text shape:\n" + canonical);
    }

    // 3. 빌드 환경 위생: 컴파일러 동작 변수를 지우고 이름을 알린다. CL=/fp:fast(clang-cl)·CCC_OVERRIDE_OPTIONS(clang driver)가
    // 실제로 fast-math를 켜는 것을 대조로 확인한 뒤, discovery 환경으로 빌드하면 영향이 없음을 확인한다.
    private static async Task EnvironmentHygieneAsync(NativeBuildRequest request, string root)
    {
        string[] names = ["CL", "LINK", "COMPILER_PATH", "CCC_OVERRIDE_OPTIONS"];
        var saved = names.ToDictionary(name => name, Environment.GetEnvironmentVariable, StringComparer.Ordinal);
        NativeToolchain dirty;
        try
        {
            Environment.SetEnvironmentVariable("CL", "/fp:fast");
            Environment.SetEnvironmentVariable("LINK", "/VERBOSE");
            Environment.SetEnvironmentVariable("COMPILER_PATH", root);
            Environment.SetEnvironmentVariable("CCC_OVERRIDE_OPTIONS", "+-ffast-math");
            dirty = await NativeToolchains.DiscoverAsync(request.Toolchain.CompilerPath);
        }
        finally
        {
            foreach ((string name, string? value) in saved)
            {
                Environment.SetEnvironmentVariable(name, value);
            }
        }

        Check(names.All(dirty.RemovedEnvironment.Contains) && dirty.RemovedEnvironment.SequenceEqual(dirty.RemovedEnvironment.Order(StringComparer.Ordinal)),
            "RemovedEnvironment must list the removed variables in order: " + string.Join(", ", dirty.RemovedEnvironment));
        Check(names.All(name => !dirty.Environment.Keys.Any(key => string.Equals(key, name, StringComparison.OrdinalIgnoreCase))),
            "removed variables must not reach the build environment");
        string? notice = NativeToolchains.EnvironmentNotice(dirty);
        Check(notice is not null && !notice.Contains('\n') && names.All(name => notice.Contains(name, StringComparison.Ordinal)),
            "environment notice must be one line naming the removed variables: " + notice);
        Check(NativeToolchains.EnvironmentNotice(dirty with { RemovedEnvironment = [] }) is null, "no notice without removed variables");
        Check(dirty.CompileArguments.SequenceEqual(request.Toolchain.CompileArguments), "removed variables must not change compile arguments");

        string fixture = Directory.CreateDirectory(Path.Combine(root, "buildinfo-hygiene")).FullName;
        string probe = Path.Combine(fixture, "probe.cpp");
        File.WriteAllText(probe, "#ifdef __FAST_MATH__\n#error compiler environment enabled fast-math\n#endif\nint main() { return 0; }\n");
        bool windows = dirty.Platform == NativePlatform.Windows;
        var control = new Dictionary<string, string?>(dirty.Environment, windows ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal)
        {
            [windows ? "CL" : "CCC_OVERRIDE_OPTIONS"] = windows ? "/fp:fast" : "+-ffast-math"
        };
        string[] arguments = [.. NativeCompileProfile.Arguments(dirty, NativeConfiguration.Debug),
            .. windows ? new[] { "/c", probe, "/Fo" + Path.Combine(fixture, "control.obj") } : ["-c", probe, "-o", Path.Combine(fixture, "control.o")]];
        var leaked = await ProcessRunner.RunAsync(new(dirty.CompilerPath, arguments, fixture, control));
        Check(leaked.ExitCode != 0 && (leaked.StdOut + leaked.StdErr).Contains("fast-math", StringComparison.Ordinal),
            "control: the environment variable must enable fast-math when inherited");
        NativeBuildRequest probeRequest = request with
        {
            Toolchain = dirty,
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = [new() { Name = "Probe", Kind = NativeModuleKind.Executable, Sources = [probe] }],
            Targets = ["Probe"]
        };
        var built = await new NativeBuildEngine().BuildAsync(probeRequest);
        Check(built.Succeeded, "build with the cleaned environment saw fast-math: " + built.Failure);

        // 허용 목록: 목록 밖 변수(실행마다 바뀌는 세션 ID 같은)는 넘기지 않고 key에도 없다. 이름은 정규 철자다.
        string random = "ROCKS_RANDOM_" + Guid.NewGuid().ToString("N");
        NativeToolchain first;
        NativeToolchain second;
        try
        {
            Environment.SetEnvironmentVariable(random, "first");
            first = await NativeToolchains.DiscoverAsync(request.Toolchain.CompilerPath);
            Environment.SetEnvironmentVariable(random, "second");
            Environment.SetEnvironmentVariable(random + "_OTHER", Guid.NewGuid().ToString("N"));
            second = await NativeToolchains.DiscoverAsync(request.Toolchain.CompilerPath);
        }
        finally
        {
            Environment.SetEnvironmentVariable(random, null);
            Environment.SetEnvironmentVariable(random + "_OTHER", null);
        }

        IReadOnlyList<string> keyed = windows ? NativeEnvironment.WindowsKeyedVariables : NativeEnvironment.MacKeyedVariables;
        HashSet<string> allowed = keyed.Concat(NativeEnvironment.HostRuntimeVariables).ToHashSet(StringComparer.Ordinal);
        Check(first.Environment.Keys.All(allowed.Contains) && !first.Environment.Keys.Any(key => key.StartsWith("ROCKS_RANDOM_", StringComparison.Ordinal)),
            "build environment must hold only allowlisted names in canonical spelling: " + string.Join(", ", first.Environment.Keys.Where(key => !allowed.Contains(key))));
        Check(first.Environment.ContainsKey("PATH") && first.Environment.Keys.Contains("PATH", StringComparer.Ordinal), "PATH must be passed under its canonical name");
        string key = Fingerprints.Toolchain(first);
        Check(key == Fingerprints.Toolchain(second) && key == Fingerprints.Toolchain(dirty),
            "variables outside the allowlist must not change the toolchain key");
        Dictionary<string, string?> With(string name, string value)
        {
            var changed = new Dictionary<string, string?>(first.Environment, windows ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal)
            {
                [name] = value
            };
            return changed;
        }

        Check(Fingerprints.Toolchain(first with { Environment = With("PATH", first.Environment["PATH"] + Path.PathSeparator + fixture) }) == key,
            "PATH is a runtime variable and must not be part of the toolchain key");
        string keyedName = windows ? "INCLUDE" : "SDKROOT";
        Check(Fingerprints.Toolchain(first with { Environment = With(keyedName, fixture) }) != key,
            keyedName + " is a keyed variable and must change the toolchain key");
        var again = await new NativeBuildEngine().BuildAsync(probeRequest with { Toolchain = second });
        Check(again.Succeeded && again.ExecutedActions == 0, "a rediscovered toolchain with different session variables must be a no-op: " + Describe(again));
    }

    // 4. FP 모델 인자·LLVM 옵션·링커 직접 인자·CL 옵션의 '-' 형태·모듈 인자 절대 경로 거부.
    private static void ModelArgumentRejection(NativeBuildRequest request, bool windows)
    {
        foreach (string argument in new[]
                 {
                     "-ffast-math", "-fno-fast-math", "/fp:fast", "-fp:fast", "/fp:precise", "/arch:AVX2", "-arch:AVX2", "-clang:-ffast-math",
                     "/clang:-ffast-math", "-ffp-contract=fast", "-ffp-model=fast", "-fno-math-errno", "-fhonor-nans", "-fno-signed-zeros",
                     "-fcomplex-arithmetic=basic", "-fdenormal-fp-math=preserve-sign", "-fexcess-precision=fast", "-fveclib=SVML",
                     "-mfpmath=sse", "-mrecip", "-mdaz-ftz", "-mlong-double-80", "-march=native", "-mcpu=apple-m1", "-mtune=generic",
                     "-mavx2", "-mfma", "-mno-sse2", "-m80387", "-msoft-float"
                 })
        {
            Check(FloatingPointArguments.IsModelArgument(argument), "model argument not detected: " + argument);
        }

        foreach (string argument in new[]
                 {
                     "-fPIC", "-fno-rtti", "-fno-exceptions", "-fsanitize=address", "-fno-omit-frame-pointer", "-fdiagnostics-color",
                     "-fdebug-compilation-dir=.", "-mmacosx-version-min=13.0", "/O2", "/clang:-std=c++23", "-FP:fast", "/FP:fast", "-O2",
                     "-machine:x64"
                 })
        {
            Check(!FloatingPointArguments.IsModelArgument(argument), "argument misdetected as an FP model argument: " + argument);
        }

        NativeBuildRequest With(string[] compile, string[] link, string[]? defines = null) => request with
        {
            Modules = request.Modules.Select(module => module.Name == "Engine"
                ? module with { AdditionalCompileArguments = compile, AdditionalLinkArguments = link, PrivateDefines = defines ?? [] }
                : module).ToArray()
        };
        void Rejects(NativeBuildRequest input, string what, string diagnostic)
        {
            try
            {
                InputValidation.Validate(input);
                throw new InvalidOperationException("accepted " + what);
            }
            catch (ArgumentException error) when (error.Message.Contains(diagnostic, StringComparison.Ordinal))
            {
            }
        }

        string absolute = windows ? "C:/Rocks/External" : "/opt/rocks/external";
        // 우회 경로(공통): cc1·전처리기 직접 인자, config 파일, -Wl, 조각의 LLVM·LTO 옵션('-' 개수 무관), 링커 선택, 대상·ABI 선택.
        string[] bypasses =
        [
            "-Xclang=-menable-no-infs", "-Xclang", "-Xpreprocessor", "--config", "--config=fast.cfg", "--config-system-dir=cfg",
            "--config-user-dir=cfg", "-Wl,--mllvm=-enable-unsafe-fp-math", "-Wl,--plugin-opt=O3", "-Wl,-plugin-opt=mcpu=native",
            "-Wl,--lto-O3", "-Wl,/opt:lldlto=0", "-fuse-ld=bfd", "--ld-path=ld.gold", "--target=x86_64-pc-linux-gnu", "-target",
            "-arch", "-fms-compatibility-version=19.00", "-fmsc-version=1900"
        ];
        string[] compileRejected =
        [
            "-ffast-math", "/fp:fast", "-fp:fast", "-ffp-contract=fast", "-fno-math-errno", "-mllvm", "-mavx2", "-march=native",
            "-Xlinker", "-Ofast", "-c", .. bypasses
        ];
        // 기존 CL 옵션의 '-' 형태(clang-cl은 두 접두를 모두 받는다 — Windows 대상에만 적용).
        string[] clForms = windows
            ? ["-clang:-ffast-math", "/mllvm:-enable-unsafe-fp-math", "-Fofoo.obj", "-Fefoo.exe", "-Fdfoo.pdb", "-FIforced.h", "-EHsc", "-GR",
               "-wd4100", "-external:W0", "-std:c++20", "-W0", "-WX-", "-TC", "-Tcfoo.c", "-clang:-O0", "-link"]
            : [];
        foreach (string argument in compileRejected.Concat(clForms))
        {
            Rejects(With([argument], []), "compile " + argument, "overrides owned output/policy");
        }

        string[] linkRejected = ["-Ofast", "-ffast-math", "-mdaz-ftz", "-Wl,-mllvm,-enable-unsafe-fp-math", "-Xlinker", .. bypasses];
        foreach (string argument in linkRejected.Concat(windows ? ["/clang:-ffast-math", "-Fefoo.exe"] : Array.Empty<string>()))
        {
            Rejects(With([], [argument]), "link " + argument, "overrides owned output/policy");
        }

        // 오탐 금지: CL 철자 표는 Windows(clang-cl) 대상에만(POSIX -Fdeps/...는 framework 경로), -m* 허용 목록은 컴파일러 인자에만
        // (링커 철자 -machine:). Android 대상은 Windows·macOS 호스트 모두에서 검증할 수 있어 POSIX 규칙을 여기서 본다.
        NativeBuildRequest posix = With(["-Fdeps/frameworks", "-fdiagnostics-color"], []) with
        {
            Toolchain = request.Toolchain with { Platform = NativePlatform.Android }
        };
        InputValidation.Validate(posix);
        InputValidation.Validate(With([], ["-machine:x64"]));
        Rejects(With(["-machine:x64"], []), "compile -machine:", "overrides owned output/policy");

        foreach ((string[] compile, string[] link) in new (string[], string[])[]
                 {
                     (["-fmacro-prefix-map=" + absolute + "=."], []), (["-iquote" + absolute], []),
                     ([], ["/LIBPATH:" + absolute]), ([], ["-L" + absolute]), ([], ["-Wl,-rpath," + absolute])
                 })
        {
            Rejects(With(compile, link), "absolute path " + string.Join(' ', compile.Concat(link)), "absolute path");
        }

        Rejects(With([], [], ["ROCKS_EXTERNAL=\"" + absolute + "\""]), "define with an absolute path", "absolute path");
        // clang-cl이 받는 GCC형 인자는 CL 옵션(/Fd 등)으로 오인하지 않는다. 수준을 바꾸지 않는 인자는 받는다.
        string[] accepted = windows ? ["-fdiagnostics-color", "-fdebug-compilation-dir=.", "/Oy-", "/Oi"] : ["-fdiagnostics-color", "-fno-omit-frame-pointer"];
        InputValidation.Validate(With(accepted, []));
        InputValidation.Validate(With([], [], ["ROCKS_RELATIVE=\"Data/Path\""]));
    }

    // 5. 내용 해시: 텍스트 확장자만 CRLF → LF(단독 CR·BOM 유지, 버퍼 경계 CR 이월), 그 밖(.lib·.a)은 원본 byte.
    private static void FileDigestRules(string root)
    {
        string directory = Directory.CreateDirectory(Path.Combine(root, "buildinfo-digests")).FullName;
        (string Raw, string Normalized) Digest(string name, byte[] content)
        {
            string path = Path.Combine(directory, name);
            File.WriteAllBytes(path, content);
            return FileDigests.Compute(path);
        }

        byte[] Bytes(string text) => Encoding.UTF8.GetBytes(text);
        var crlfHeader = Digest("crlf.h", Bytes("a\r\nb\r\n"));
        var lfHeader = Digest("lf.h", Bytes("a\nb\n"));
        Check(crlfHeader.Normalized == lfHeader.Normalized && crlfHeader.Raw != lfHeader.Raw, "header CRLF must normalize to LF");
        Check(lfHeader.Raw == Hashing.Sha256File(Path.Combine(directory, "lf.h")), "raw digest must equal the action-key file hash");
        foreach (string extension in new[] { ".cpp", ".cc", ".cxx", ".c++", ".m", ".mm", ".c", ".inl", ".HPP" })
        {
            Check(Digest("crlf" + extension, Bytes("x\r\n")).Normalized == Digest("lf" + extension, Bytes("x\n")).Normalized,
                "text extension not normalized: " + extension);
        }

        foreach (string extension in new[] { ".lib", ".a", ".txt", ".json" })
        {
            var crlf = Digest("crlf" + extension, Bytes("x\r\n"));
            Check(crlf.Normalized == crlf.Raw && crlf.Normalized != Digest("lf" + extension, Bytes("x\n")).Normalized,
                "binary or unknown extension must keep raw bytes: " + extension);
        }

        Check(Digest("lone.h", Bytes("a\rb")).Normalized != Digest("joined.h", Bytes("ab")).Normalized &&
            Digest("lone2.h", Bytes("a\rb")).Normalized == Digest("lone2.lib", Bytes("a\rb")).Normalized, "lone CR must be kept");
        Check(Digest("bom.h", [0xef, 0xbb, 0xbf, (byte)'x']).Normalized != Digest("nobom.h", Bytes("x")).Normalized, "BOM must be kept");
        Check(Digest("tail.h", Bytes("x\r")).Normalized == Digest("tail.lib", Bytes("x\r")).Normalized, "trailing CR must be kept");
        // 버퍼 경계: CR이 한 버퍼의 마지막 byte이고 LF가 다음 버퍼의 첫 byte다.
        byte[] boundary = new byte[FileDigests.BufferSize + 2];
        boundary.AsSpan(0, FileDigests.BufferSize - 1).Fill((byte)'a');
        boundary[FileDigests.BufferSize - 1] = (byte)'\r';
        boundary[FileDigests.BufferSize] = (byte)'\n';
        boundary[FileDigests.BufferSize + 1] = (byte)'b';
        byte[] joined = [.. boundary.AsSpan(0, FileDigests.BufferSize - 1), (byte)'\n', (byte)'b'];
        Check(Digest("boundary.h", boundary).Normalized == Digest("boundary-lf.h", joined).Normalized, "CRLF across the buffer boundary");
        byte[] loneBoundary = (byte[])boundary.Clone();
        loneBoundary[FileDigests.BufferSize] = (byte)'c';
        Check(Digest("lone-boundary.h", loneBoundary).Normalized == Digest("lone-boundary.lib", loneBoundary).Raw,
            "lone CR at the buffer boundary must be kept");
    }

    // 6. 실제 빌드: 가짜 Core(표지·BuildInfo.h·최소 Sha256Digest) ← Engine ← Game(TU 2개). 출력한 digest가 C# 계산과 같고, 표지 없는
    // Core에는 주입하지 않으며, 증분·줄 끝 규칙을 지킨다.
    private static async Task InjectionAndIncrementalAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "buildinfo-injection")).FullName;
        WriteFixture(fixture);
        NativeBuildRequest game = FixtureRequest(request, fixture, Path.Combine(fixture, "out"));
        var builder = new NativeBuildEngine();
        NativeBuildResult cold = await Succeeded(builder, game, "BuildInfo cold build");
        Check(cold.ExecutedActions == 6, "cold build must run Engine compile/archive, two Game TUs, the generated TU, and link: " + Describe(cold));
        string directory = Path.Combine(game.OutputDirectory, "BuildInfo");
        string manifestPath = Path.Combine(directory, "Game.BuildInfo.txt");
        string sourcePath = Path.Combine(directory, "Game.BuildInfo.cpp");
        string fingerprint = Convert.ToHexStringLower(ToolchainFingerprint.Compute(request.Toolchain));
        string id = Hashing.Sha256File(manifestPath);
        Check(cold.ToolchainFingerprint == fingerprint && cold.ToolBuildIds.Count == 1 && cold.ToolBuildIds["Game"] == id,
            "result fingerprint/ID must equal the C# fingerprint and sha256(Game.BuildInfo.txt)");
        Check(File.ReadAllText(Path.Combine(directory, "ToolchainFingerprint.txt")) == ToolchainFingerprint.CanonicalText(request.Toolchain),
            "ToolchainFingerprint.txt must hold the canonical text");
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            var run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            Check(run.ExitCode == 0, "BuildInfo game returned " + run.ExitCode + ": " + run.StdErr);
            Check(run.StdOut.Replace("\r", "", StringComparison.Ordinal) == $"fingerprint={fingerprint}\nid={id}\n",
                "printed digests differ from the C# fingerprint and manifest digest:\n" + run.StdOut);
        }

        string manifest = File.ReadAllText(manifestPath);
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        Check(manifest.StartsWith("rocks-tool-build-id 1\nexecutable: Game\ntoolchain-fingerprint: " + fingerprint + "\ntoolchain-target: " +
            request.Toolchain.TargetTriple + "\ntoolchain-abi: " + string.Join(';', request.Toolchain.AbiFields) + "\nhost-dependent: no\n",
            StringComparison.Ordinal), "manifest header:\n" + manifest);
        foreach (string expected in new[]
                 {
                     "action: Archive Engine\n", "action: Compile src:engine/src/engine.cpp\n", "action: Compile src:game/helper.cpp\n",
                     "action: Compile src:game/main.cpp\n", "action: Link Game\n", "arg: obj:src:game/main.cpp\n", "arg: out:lib/Engine." ,
                     (windows ? "arg: /Isrc:core/include\n" : "arg: -Isrc:core/include\n"), "input: src:core/include/Core/Build/BuildInfo.h ",
                     "input: src:engine/src/engine.cpp ", "input: src:game/main.cpp ",
                     (windows ? "arg: /Isrc:game/pct%25dir\n" : "arg: -Isrc:game/pct%25dir\n"), "input: src:game/pct%25dir/pct%25name.h "
                 })
        {
            Check(manifest.Contains(expected, StringComparison.Ordinal), $"manifest lacks '{expected.TrimEnd()}':\n" + manifest);
        }

        Check(!manifest.Contains("%2525", StringComparison.Ordinal), "path keys must be percent-encoded once:\n" + manifest);

        Check(!manifest.Contains("BuildInfo.cpp", StringComparison.Ordinal) && !manifest.Contains(NativeToolchains.NoDefaultConfig, StringComparison.Ordinal) &&
            !manifest.Contains(fixture, StringComparison.OrdinalIgnoreCase) && !manifest.Contains(fixture.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase) &&
            manifest.IndexOf("action: Archive", StringComparison.Ordinal) < manifest.IndexOf("action: Compile", StringComparison.Ordinal) &&
            manifest.IndexOf("action: Compile", StringComparison.Ordinal) < manifest.IndexOf("action: Link", StringComparison.Ordinal) &&
            manifest.IndexOf("action: Link", StringComparison.Ordinal) < manifest.IndexOf("input: ", StringComparison.Ordinal),
            "manifest must exclude itself, toolchain arguments and host paths, and keep the action order:\n" + manifest);
        string[] inputKeys = manifest.Split('\n').Where(line => line.StartsWith("input: ", StringComparison.Ordinal))
            .Select(line => line[7..line.LastIndexOf(' ')]).ToArray();
        Check(inputKeys.SequenceEqual(inputKeys.Order(StringComparer.Ordinal)) && inputKeys.Distinct().Count() == inputKeys.Length,
            "manifest inputs must be sorted and unique");
        string source = File.ReadAllText(sourcePath);
        Check(source.StartsWith("// Generated by Rocks.Build BuildInfo 1 for module Game. Do not edit.\n#include <Core/Build/BuildInfo.h>\n", StringComparison.Ordinal) &&
            !source.Contains('\r') && source.All(char.IsAscii) && source.Contains("0x" + fingerprint[..2] + ", 0x" + fingerprint[2..4], StringComparison.Ordinal),
            "generated TU shape:\n" + source);

        // no-op: 실행 액션 0, 생성 파일을 다시 쓰지 않는다.
        DateTime written = File.GetLastWriteTimeUtc(sourcePath);
        NativeBuildResult warm = await Succeeded(builder, game, "BuildInfo no-op");
        Check(warm.ExecutedActions == 0 && File.GetLastWriteTimeUtc(sourcePath) == written && warm.ToolBuildIds["Game"] == id,
            "no-op must execute nothing and keep the generated TU: " + Describe(warm));

        // Engine 소스 편집: Engine compile·archive, 생성 TU, link만 다시 돈다(Game의 다른 TU는 재사용).
        string engineSource = Path.Combine(fixture, "engine", "src", "engine.cpp");
        File.AppendAllText(engineSource, "int EngineExtra() noexcept { return 1; }\n");
        NativeBuildResult edited = await Succeeded(builder, game, "BuildInfo engine edit");
        string generatedId = "Game:compile:" + Fingerprints.Strings([sourcePath])[..20];
        string[] executed = edited.Actions.Where(action => action.Executed).Select(action => action.Id).Order(StringComparer.Ordinal).ToArray();
        string[] expectedIds = edited.Actions.Where(action => action.Module == "Engine" || action.Id == generatedId || action.Kind == "Link")
            .Select(action => action.Id).Order(StringComparer.Ordinal).ToArray();
        Check(executed.SequenceEqual(expectedIds) && executed.Length == 4, "engine edit must rerun only Engine, the generated TU, and link: " + Describe(edited));
        Check(edited.ToolBuildIds["Game"] != id, "engine edit must change the tool build ID");
        string editedId = edited.ToolBuildIds["Game"];

        // 줄 끝: 소스를 CRLF로 바꿔도 ID가 같고 생성 TU는 실행되지 않는다.
        File.WriteAllText(engineSource, File.ReadAllText(engineSource).Replace("\n", "\r\n", StringComparison.Ordinal));
        NativeBuildResult crlf = await Succeeded(builder, game, "BuildInfo CRLF source");
        Check(crlf.ToolBuildIds["Game"] == editedId && !crlf.Actions.Single(action => action.Id == generatedId).Executed &&
            crlf.Actions.Any(action => action.Module == "Engine" && action.Kind == "Compile" && action.Executed),
            "CRLF conversion must keep the ID and skip the generated TU: " + Describe(crlf));

        // 이름이 Core여도 표지가 없으면 주입하지 않는다: 계획은 기존과 같은 5 액션이고 정의가 없어 link가 실패한다(주입되지 않은 링크에서
        // 부르면 링크 오류라는 헤더 계약).
        NativeBuildRequest unmarked = game with
        {
            OutputDirectory = Path.Combine(fixture, "out-unmarked"),
            Modules = game.Modules.Select(module => module with { ProvidesBuildInfo = false }).ToArray()
        };
        NativeBuildResult plain = await builder.BuildAsync(unmarked);
        NativeActionResult plainLink = plain.Actions.Single(action => action.Kind == "Link");
        Check(!plain.Succeeded && plain.Actions.Count == 5 && plain.ToolchainFingerprint is null && plain.ToolBuildIds.Count == 0 &&
            !plainLink.Succeeded && (plainLink.StdOut + plainLink.StdErr).Contains("ToolchainFingerprint", StringComparison.Ordinal) &&
            !Directory.Exists(Path.Combine(unmarked.OutputDirectory, "BuildInfo")), "a module named Core without the marker must not inject: " + Describe(plain));

        // Core에 닿지 않는 기존 픽스처는 액션 수가 그대로이고 BuildInfo를 만들지 않는다.
        NativeBuildResult existing = await Succeeded(builder, request with { OutputDirectory = Path.Combine(fixture, "out-existing") }, "existing fixture");
        Check(existing.ExecutedActions == 4 && existing.ToolchainFingerprint is null &&
            !Directory.Exists(Path.Combine(fixture, "out-existing", "BuildInfo")), "fixture without Core must keep its action count: " + Describe(existing));
    }

    // 7. 호스트 무관: 이름이 다른 두 루트(한글·공백), 다른 출력 이름, subst 드라이브 루트, 8.3 경로, junction에서 ID·생성 TU byte가 같다.
    private static async Task HostIndependenceAsync(NativeBuildRequest request, string root)
    {
        string first = Path.Combine(root, "빌드 정보 가");
        string second = Path.Combine(root, "build info second");
        WriteFixture(first);
        WriteFixture(second);
        var builder = new NativeBuildEngine();
        async Task<(string Id, byte[] Source)> Build(string fixture, string output, string name)
        {
            NativeBuildResult result = await Succeeded(builder, FixtureRequest(request, fixture, output), name);
            return (result.ToolBuildIds["Game"], File.ReadAllBytes(Path.Combine(output, "BuildInfo", "Game.BuildInfo.cpp")));
        }

        var baseline = await Build(first, Path.Combine(first, "out-a"), "first root");
        void Same((string Id, byte[] Source) other, string name)
        {
            Check(other.Id == baseline.Id && other.Source.SequenceEqual(baseline.Source), name + " changed the tool build ID or generated TU");
        }

        Same(await Build(second, Path.Combine(second, "output second"), "second root"), "a different root and output name");
        if (!OperatingSystem.IsWindows())
        {
            string link = Path.Combine(root, "buildinfo-symlink");
            Directory.CreateSymbolicLink(link, first);
            Same(await Build(link, Path.Combine(link, "out-link"), "symlink root"), "a symbolic link root");
            return;
        }

        // 루트 철자의 대소문자 차이(c:\ 대 C:\).
        string lowerDrive = char.ToLowerInvariant(first[0]) + first[1..];
        Same(await Build(lowerDrive, Path.Combine(lowerDrive, "out-case"), "lowercase drive root"), "a lowercase drive letter");

        // subst 드라이브 루트(R:\ 같은 형태).
        var used = DriveInfo.GetDrives().Select(drive => char.ToUpperInvariant(drive.Name[0])).ToHashSet();
        char letter = Enumerable.Range('K', 'Z' - 'K' + 1).Select(value => (char)value).Reverse().First(value => !used.Contains(value));
        string drive = letter + ":";
        var mapped = await ProcessRunner.RunAsync(new("cmd.exe", ["/d", "/c", "subst", drive, first], root));
        Check(mapped.ExitCode == 0, "subst failed: " + mapped.StdOut + mapped.StdErr);
        try
        {
            Same(await Build(drive + "\\", drive + "\\out-subst", "subst root"), "a subst drive root");
        }
        finally
        {
            await ProcessRunner.RunAsync(new("cmd.exe", ["/d", "/c", "subst", drive, "/D"], root));
        }

        // 8.3 짧은 경로(볼륨에서 8.3 이름을 끄면 긴 경로와 같아 의미가 없으므로 그때는 확인을 생략한다).
        string shortPath = ShortPath(first);
        if (!string.Equals(shortPath, first, StringComparison.OrdinalIgnoreCase))
        {
            Same(await Build(shortPath, Path.Combine(shortPath, "out-short"), "8.3 root"), "an 8.3 short root");
        }
        else
        {
            Console.WriteLine("NOTE BuildInfo: 8.3 short names are disabled on this volume; the short-path case was not exercised.");
        }

        // junction.
        string junction = Path.Combine(root, "buildinfo-junction");
        var linked = await ProcessRunner.RunAsync(new("cmd.exe", ["/d", "/c", "mklink", "/J", junction, first], root));
        Check(linked.ExitCode == 0, "mklink /J failed: " + linked.StdOut + linked.StdErr);
        try
        {
            Same(await Build(junction, Path.Combine(junction, "out-junction"), "junction root"), "a junction root");
        }
        finally
        {
            Directory.Delete(junction);
        }
    }

    // 8. 구성(Debug·Release·Asan 지문 같고 ID 다름), 외부 include·prebuilt의 ext 키, include inventory 제외, 실패한 link, 빈 신원 거부.
    private static async Task ConfigurationsAndFailuresAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "buildinfo-more")).FullName;
        WriteFixture(fixture);
        var builder = new NativeBuildEngine();
        NativeBuildRequest game = FixtureRequest(request, fixture, Path.Combine(fixture, "out-debug"));
        var configurations = new List<NativeConfiguration> { NativeConfiguration.Debug, NativeConfiguration.Release };
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            configurations.Add(NativeConfiguration.Asan);
        }

        var results = new List<NativeBuildResult>();
        foreach (NativeConfiguration configuration in configurations)
        {
            results.Add(await Succeeded(builder, game with
            {
                Configuration = configuration,
                OutputDirectory = Path.Combine(fixture, "out-" + configuration.ToString().ToLowerInvariant())
            }, "configuration " + configuration));
        }

        Check(results.All(result => result.ToolchainFingerprint == results[0].ToolchainFingerprint),
            "configurations must share the toolchain fingerprint");
        Check(results.Select(result => result.ToolBuildIds["Game"]).Distinct().Count() == results.Count,
            "configurations must produce different tool build IDs");

        // 루트 밖 include 디렉터리(여럿 — 하나는 다른 것 안)와 prebuilt 라이브러리: ext 키와 host-dependent: yes. 순번은 include 순서이고
        // 겹친 루트 아래 파일은 가장 긴 루트의 키를 쓴다. 빌드는 막지 않는다.
        string external = Directory.CreateDirectory(Path.Combine(root, "buildinfo-external")).FullName;
        string externalInclude = Directory.CreateDirectory(Path.Combine(external, "include")).FullName;
        string externalNested = Directory.CreateDirectory(Path.Combine(externalInclude, "nested")).FullName;
        string externalSecond = Directory.CreateDirectory(Path.Combine(external, "second")).FullName;
        File.WriteAllText(Path.Combine(externalInclude, "external.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(externalNested, "inner.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(externalSecond, "second.h"), "#pragma once\n");
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        string prebuilt = Path.Combine(external, windows ? "ExternalEngine.lib" : "libExternalEngine.a");
        File.Copy(results[0].ModuleOutputs["Engine"], prebuilt, true);
        NativeBuildRequest externalGame = game with
        {
            OutputDirectory = Path.Combine(fixture, "out-external"),
            Modules = game.Modules.Select(module => module.Name == "Game"
                ? module with { PrivateIncludeDirectories = [externalInclude, externalNested, externalSecond], PrebuiltLibraries = [prebuilt] }
                : module).ToArray()
        };
        await Succeeded(builder, externalGame, "external inputs");
        string manifest = File.ReadAllText(Path.Combine(externalGame.OutputDirectory, "BuildInfo", "Game.BuildInfo.txt"));
        string include = windows ? "arg: /I" : "arg: -I";
        foreach (string expected in new[]
                 {
                     "host-dependent: yes\n", include + "ext0:\n", include + "ext1:\n", include + "ext2:\n", "input: ext0:external.h ",
                     "input: ext1:inner.h ", "input: ext2:second.h ",
                     "arg: ext:" + Path.GetFileName(prebuilt) + "\n", "input: ext:" + Path.GetFileName(prebuilt) + " " + Hashing.Sha256File(prebuilt) + "\n"
                 })
        {
            Check(manifest.Contains(expected, StringComparison.Ordinal), $"external manifest lacks '{expected.TrimEnd()}':\n" + manifest);
        }

        Check(manifest.IndexOf(include + "ext0:\n", StringComparison.Ordinal) < manifest.IndexOf(include + "ext1:\n", StringComparison.Ordinal) &&
            manifest.IndexOf(include + "ext1:\n", StringComparison.Ordinal) < manifest.IndexOf(include + "ext2:\n", StringComparison.Ordinal) &&
            !manifest.Contains("ext0:nested/", StringComparison.Ordinal), "ext<i> must follow the include order and the longest root:\n" + manifest);
        Check(!manifest.Contains(external, StringComparison.OrdinalIgnoreCase), "external manifest contains the external absolute path");

        // include inventory 제외: 점 디렉터리, .git(파일)을 가진 중첩 작업 사본, .native-build.lock·.rocks-invocation.lock을 가진 출력 디렉터리.
        string engineInclude = Path.Combine(fixture, "engine", "include");
        string dot = Directory.CreateDirectory(Path.Combine(engineInclude, ".claude", "worktrees")).FullName;
        string nested = Directory.CreateDirectory(Path.Combine(engineInclude, "nested-copy")).FullName;
        string output = Directory.CreateDirectory(Path.Combine(engineInclude, "user-output")).FullName;
        string invocation = Directory.CreateDirectory(Path.Combine(engineInclude, "cli-output")).FullName;
        File.WriteAllText(Path.Combine(dot, "dot.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(nested, ".git"), "gitdir: ../.git/worktrees/nested\n");
        File.WriteAllText(Path.Combine(nested, "nested.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(output, ".native-build.lock"), "");
        File.WriteAllText(Path.Combine(output, "output.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(invocation, ".rocks-invocation.lock"), "");
        File.WriteAllText(Path.Combine(invocation, "invocation.h"), "#pragma once\n");
        string[] headers = Fingerprints.TreeFiles(engineInclude, true).Select(Path.GetFileName).ToArray()!;
        string[] all = Fingerprints.TreeFiles(engineInclude, false).Select(Path.GetFileName).ToArray()!;
        Check(headers.SequenceEqual(["engine.h"]), "headersOnly inventory must skip dot, nested-copy and output directories: " + string.Join(", ", headers));
        Check(all.Contains("dot.h") && all.Contains("nested.h") && all.Contains("output.h") && all.Contains("invocation.h"),
            "declared-input enumeration must not change");
        NativeBuildResult excluded = await Succeeded(builder, game, "inventory exclusions");
        Check(excluded.ExecutedActions == 0, "excluded directories must not invalidate actions: " + Describe(excluded));
        foreach (string edited in new[] { Path.Combine(dot, "dot.h"), Path.Combine(nested, "nested.h"), Path.Combine(output, "output.h"),
                     Path.Combine(invocation, "invocation.h") })
        {
            File.AppendAllText(edited, "// edited\n");
        }

        Check((await Succeeded(builder, game, "inventory edits")).ExecutedActions == 0, "edits in excluded directories rebuilt actions");
        string excludedManifest = File.ReadAllText(Path.Combine(game.OutputDirectory, "BuildInfo", "Game.BuildInfo.txt"));
        Check(new[] { "dot.h", "nested.h", "output.h", "invocation.h" }.All(name => !excludedManifest.Contains(name, StringComparison.Ordinal)),
            "manifest contains excluded headers");

        // 실패한 link는 ToolBuildIds에 없다(지문은 알린다).
        string broken = Path.Combine(fixture, "game", "broken.cpp");
        File.WriteAllText(broken, "int RocksMissingSymbol() noexcept;\nint RocksUseMissing() noexcept { return RocksMissingSymbol(); }\n");
        NativeBuildRequest failing = game with
        {
            OutputDirectory = Path.Combine(fixture, "out-failing"),
            Modules = game.Modules.Select(module => module.Name == "Game" ? module with { Sources = [.. module.Sources, broken] } : module).ToArray()
        };
        NativeBuildResult failed = await builder.BuildAsync(failing);
        Check(!failed.Succeeded && failed.Actions.Any(action => action.Kind == "Link" && action.Executed && !action.Succeeded) &&
            failed.ToolchainFingerprint == results[0].ToolchainFingerprint && !failed.ToolBuildIds.ContainsKey("Game"),
            "a failed link must not report a tool build ID: " + Describe(failed));

        // 수동 툴체인의 빈 신원 필드·툴체인 소유 루트와 소스 루트의 겹침은 출력을 쓰기 전에 거부한다.
        foreach ((string name, NativeToolchain invalid) in new[]
                 {
                     ("CompilerVersion", request.Toolchain with { CompilerVersion = "" }),
                     ("TargetTriple", request.Toolchain with { TargetTriple = "" }),
                     ("CompilerDigest", request.Toolchain with { CompilerDigest = "" }),
                     ("LinkerDigest", request.Toolchain with { LinkerDigest = "ABC" }),
                     ("OwnedRoots", request.Toolchain with { OwnedRoots = new Dictionary<string, string> { ["resource"] = fixture } })
                 })
        {
            string invalidOutput = Path.Combine(fixture, "out-invalid-" + name);
            try
            {
                await builder.BuildAsync(game with { Toolchain = invalid, OutputDirectory = invalidOutput });
                throw new InvalidOperationException("incomplete toolchain identity accepted: " + name);
            }
            catch (ArgumentException error) when (error.Message.Contains(name, StringComparison.Ordinal))
            {
            }

            Check(!Directory.Exists(Path.Combine(invalidOutput, "BuildInfo")), "rejected identity wrote BuildInfo: " + name);
        }
    }

    // 가짜 Core(header-only, 표지·BuildInfo.h·최소 Sha256Digest) ← Engine(정적 라이브러리) ← Game(실행 파일, TU 2개).
    private static void WriteFixture(string fixture)
    {
        void Write(string relative, string text)
        {
            string path = Path.Combine(fixture, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text);
        }

        Write("core/include/Core/Build/BuildInfo.h",
            "#pragma once\n" +
            "namespace rocks {\n" +
            "struct Sha256Digest {\n" +
            "    static constexpr unsigned Bytes = 32;\n" +
            "    unsigned char bytes[Bytes] = {};\n" +
            "};\n" +
            "namespace build {\n" +
            "Sha256Digest ToolchainFingerprint() noexcept;\n" +
            "Sha256Digest ToolBuildId() noexcept;\n" +
            "} // namespace build\n" +
            "} // namespace rocks\n");
        Write("engine/include/engine.h", "#pragma once\nint EngineValue() noexcept;\n");
        Write("engine/src/engine.cpp", "#include \"engine.h\"\n#include <Core/Build/BuildInfo.h>\nint EngineValue() noexcept { return 7; }\n");
        Write("game/helper.cpp", "int Helper() noexcept;\nint Helper() noexcept { return 3; }\n");
        // '%'가 든 include 디렉터리·헤더: 경로 키의 percent-encoding이 한 번만 적용되는지 본다.
        Write("game/pct%dir/pct%name.h", "#pragma once\n");
        Write("game/main.cpp",
            "#include <Core/Build/BuildInfo.h>\n" +
            "#include <stdio.h>\n" +
            "#include \"engine.h\"\n" +
            "int Helper() noexcept;\n" +
            "static void Print(const char* name, const rocks::Sha256Digest& digest) {\n" +
            "    printf(\"%s=\", name);\n" +
            "    for (unsigned index = 0; index < rocks::Sha256Digest::Bytes; ++index) {\n" +
            "        printf(\"%02x\", static_cast<unsigned>(digest.bytes[index]));\n" +
            "    }\n" +
            "    printf(\"\\n\");\n" +
            "}\n" +
            "int main() {\n" +
            "    Print(\"fingerprint\", rocks::build::ToolchainFingerprint());\n" +
            "    Print(\"id\", rocks::build::ToolBuildId());\n" +
            "    return EngineValue() + Helper() == 10 ? 0 : 1;\n" +
            "}\n");
    }

    private static NativeBuildRequest FixtureRequest(NativeBuildRequest template, string fixture, string output) => template with
    {
        RootDirectory = fixture,
        OutputDirectory = output,
        Configuration = NativeConfiguration.Debug,
        Targets = ["Game"],
        Modules =
        [
            new() { Name = "Core", PublicIncludeDirectories = [Path.Combine(fixture, "core", "include")], ProvidesBuildInfo = true },
            new()
            {
                Name = "Engine", Sources = [Path.Combine(fixture, "engine", "src", "engine.cpp")],
                PublicIncludeDirectories = [Path.Combine(fixture, "engine", "include")], Dependencies = ["Core"]
            },
            new()
            {
                Name = "Game", Kind = NativeModuleKind.Executable, Dependencies = ["Engine"],
                Sources = [Path.Combine(fixture, "game", "main.cpp"), Path.Combine(fixture, "game", "helper.cpp")],
                PrivateIncludeDirectories = [Path.Combine(fixture, "game", "pct%dir")]
            }
        ]
    };

    private static NativeToolchain Manual(NativePlatform platform, string triple) => new()
    {
        Platform = platform,
        CompilerPath = "C:/LLVM/bin/clang-cl.exe",
        ArchiverPath = "C:/LLVM/bin/llvm-lib.exe",
        Identity = "clang version 21.1.6\nInstalledDir: C:/LLVM/bin",
        CompilerVersion = "clang version 21.1.6",
        TargetTriple = triple,
        CompilerDigest = LowerA,
        LinkerDigest = LowerB
    };

    private static async Task<NativeBuildResult> Succeeded(NativeBuildEngine builder, NativeBuildRequest request, string name)
    {
        NativeBuildResult result = await builder.BuildAsync(request);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(name + " failed: " + result.Failure + "\n" +
                string.Join("\n", result.Actions.Select(action => action.StdOut + action.StdErr)));
        }

        return result;
    }

    private static string Describe(NativeBuildResult result) =>
        string.Join(", ", result.Actions.Select(action => $"{action.Id}:executed={action.Executed}"));

    private static string ShortPath(string path)
    {
        char[] buffer = new char[32768];
        uint length = GetShortPathNameW(path, buffer, (uint)buffer.Length);
        return length == 0 || length >= buffer.Length ? path : new string(buffer, 0, (int)length);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetShortPathNameW(string longPath, char[] shortPath, uint bufferLength);

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
