using Rocks.Tooling.Common;
using System.Runtime.InteropServices;
using System.Text;

namespace Rocks.Build;

public static class NativeToolchains
{
    /// <summary>
    /// 기본 config 파일(Homebrew LLVM의 <c>etc/clang/*.cfg</c> 등)을 끄는 인자다. discovery가 받는 컴파일러면 모든 드라이버의
    /// CompileArguments·LinkArguments와 probe 맨 앞에 넣는다. 접두 없이 쓴다 — clang-cl도 이 철자를 직접 받고,
    /// <c>/clang:--no-default-config</c>는 config를 읽은 뒤 해석되어 쓰이지 않은 인자 경고(<c>/WX</c>에서 오류)가 된다.
    /// </summary>
    public const string NoDefaultConfig = "--no-default-config";

    public static async Task<NativeToolchain> DiscoverAsync(string? compiler = null,
        CancellationToken cancellationToken = default)
    {
        var reported = await NativeEnvironment.CreateReportedAsync(compiler, cancellationToken);
        var environment = reported.Variables;
        bool windows = OperatingSystem.IsWindows();
        if (!windows && !OperatingSystem.IsMacOS())
        {
            throw new PlatformNotSupportedException("Only Windows and macOS native hosts are supported.");
        }

        string compilerPath = NativeEnvironment.FindExecutable(compiler ?? (windows ? "clang-cl" : "clang++"), environment);
        (ProcessResult version, string[] config) = await ProbeVersionAsync(compilerPath, environment, cancellationToken);
        string compilerVersion = ParseCompilerVersion(version.StdOut);
        // archiver·linker는 compiler와 같은 LLVM 설치에서 먼저 찾는다. Release의 ThinLTO bitcode는 같은 LLVM
        // 버전의 도구만 확실히 읽으며, clang-cl의 -fuse-ld=lld도 자기 폴더의 lld-link를 쓴다. macOS에서
        // compiler 옆에 llvm-ar가 없으면(Apple clang) PATH의 다른 llvm-ar 대신 Xcode의 ar(libLTO)를 쓴다.
        string compilerDirectory = Path.GetDirectoryName(compilerPath)!;
        string archiver = FindBeside(compilerDirectory, windows ? "llvm-lib.exe" : "llvm-ar")
            ?? (windows ? FindFirst(environment, ["llvm-lib", "lib.exe"]) : "/usr/bin/ar");
        string? linker = windows ? FindBeside(compilerDirectory, "lld-link.exe") ?? FindFirst(environment, ["lld-link", "link.exe"]) : null;
        var paths = new List<string>();
        var runtime = new List<string>();
        var compile = new List<string>(config);
        var link = new List<string>(config);
        var owned = new Dictionary<string, string>(StringComparer.Ordinal);
        var abiFields = new List<string> { "compiler=" + compilerVersion };
        string EnvironmentValue(string name) => environment.FirstOrDefault(pair => pair.Key.Equals(name,
            windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value ?? "";
        var resource = await ProcessRunner.RunAsync(new ProcessSpec(compilerPath,
            [.. config, windows ? "/clang:-print-resource-dir" : "-print-resource-dir"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (resource.ExitCode != 0)
        {
            throw new InvalidOperationException("Cannot identify the compiler resource directory.");
        }

        string resourcePath = resource.StdOut.Trim();
        if (Directory.Exists(resourcePath))
        {
            resourcePath = Path.GetFullPath(resourcePath);
            paths.Add(resourcePath);
            owned["resource"] = resourcePath;
            if (windows)
            {
                // Windows host contract는 x64다. resource tree 전체를 열거하면 i686 DLL도 같은
                // 파일명으로 수집되어 잘못 배포되므로 compiler가 사용하는 x64 디렉터리만 고른다.
                foreach (string relative in new[] { "lib/x86_64-unknown-windows-msvc", "lib/x86_64-pc-windows-msvc", "lib/windows" })
                {
                    string directory = Path.Combine(resourcePath, relative);
                    if (!Directory.Exists(directory))
                    {
                        continue;
                    }

                    string[] files = Directory.EnumerateFiles(directory, "clang_rt.asan_dynamic*.dll").ToArray();
                    if (files.Length == 0)
                    {
                        continue;
                    }

                    runtime.AddRange(files.Where(file => !Path.GetFileName(file).Contains("i386", StringComparison.OrdinalIgnoreCase)));
                    owned["runtime"] = Path.GetFullPath(directory);
                    break;
                }
            }
        }

        var sdkAbi = new List<string>();
        string? xcrun = null;
        if (windows)
        {
            string sdkVersion = EnvironmentValue("WindowsSDKVersion").TrimEnd('/', '\\');
            string toolsVersion = EnvironmentValue("VCToolsVersion").TrimEnd('/', '\\');
            sdkAbi.Add("WindowsSDKVersion=" + sdkVersion);
            sdkAbi.Add("VCToolsVersion=" + toolsVersion);
            abiFields.Add("WindowsSDKVersion=" + sdkVersion);
            abiFields.Add("VCToolsVersion=" + toolsVersion);
            // SDK 환경의 실제 include/lib 내용도 key에 넣어 같은 경로에서 교체된 SDK를 구별한다.
            foreach (string variable in new[] { "INCLUDE", "LIB" })
            {
                string? value = environment.FirstOrDefault(pair => pair.Key.Equals(variable, StringComparison.OrdinalIgnoreCase)).Value;
                foreach (string directory in (value ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                {
                    if (Directory.Exists(directory))
                    {
                        paths.Add(directory);
                    }
                }
            }
        }
        else
        {
            xcrun = NativeEnvironment.FindExecutable("xcrun", environment);
            var sdk = await ProcessRunner.RunAsync(new ProcessSpec(xcrun, ["--show-sdk-path"],
                Directory.GetCurrentDirectory(), environment), cancellationToken);
            if (sdk.ExitCode != 0 || !Directory.Exists(sdk.StdOut.Trim()))
            {
                throw new InvalidOperationException("Cannot identify the macOS SDK.");
            }

            string sdkPath = sdk.StdOut.Trim();
            foreach ((string option, string key) in new[] { ("--show-sdk-version", "sdk-version"), ("--show-sdk-build-version", "sdk-build-version") })
            {
                var sdkVersion = await ProcessRunner.RunAsync(new ProcessSpec(xcrun, [option],
                    Directory.GetCurrentDirectory(), environment), cancellationToken);
                if (sdkVersion.ExitCode != 0 || string.IsNullOrWhiteSpace(sdkVersion.StdOut))
                {
                    throw new InvalidOperationException("Cannot identify macOS SDK ABI: " + option);
                }

                sdkAbi.Add(option + "=" + sdkVersion.StdOut.Trim());
                abiFields.Add(key + "=" + sdkVersion.StdOut.Trim());
            }

            string deployment = EnvironmentValue("MACOSX_DEPLOYMENT_TARGET");
            if (deployment.Length == 0)
            {
                deployment = "13.0";
            }

            if (!Version.TryParse(deployment, out var minimum) || minimum.Major < 13 ||
                deployment.Any(value => !char.IsAsciiDigit(value) && value != '.'))
            {
                throw new ArgumentException("MACOSX_DEPLOYMENT_TARGET must be a numeric version >= 13.0.");
            }

            environment["MACOSX_DEPLOYMENT_TARGET"] = deployment;
            compile.Add("-mmacosx-version-min=" + deployment);
            link.Add("-mmacosx-version-min=" + deployment);
            sdkAbi.Add("deployment=" + deployment);
            abiFields.Add("deployment=" + deployment);
            compile.AddRange(["-isysroot", sdkPath]);
            link.AddRange(["-isysroot", sdkPath]);
            paths.Add(sdkPath);
            owned["sysroot"] = Path.GetFullPath(sdkPath);
            // Apple ar의 deterministic archive 모드. 환경 값도 action key에 포함된다.
            environment["ZERO_AR_DATE"] = "1";
        }

        // 대상은 실제 compile 인자(배포 대상·sysroot 포함)를 준 effective triple이다. `--version`의 Target: 줄은 Apple clang에서
        // 호스트 커널 버전을 담으므로 쓰지 않는다.
        string triple = await ProbeTargetTripleAsync(compilerPath, compile, windows, environment, cancellationToken);
        string compilerDigest;
        string linkerDigest;
        if (windows)
        {
            compilerDigest = Hashing.Sha256File(compilerPath);
            linkerDigest = Hashing.Sha256File(linker!);
        }
        else
        {
            (compilerDigest, linkerDigest) = await MacBinaryDigestsAsync(compilerPath, xcrun!, environment, cancellationToken);
        }

        var abi = new List<string> { "rocks-native-abi-v2", windows ? "Windows" : "MacOS", RuntimeInformation.ProcessArchitecture.ToString() };
        abi.AddRange(AbiVersionLines(version.StdOut, compilerVersion, triple));
        abi.AddRange(sdkAbi);
        abiFields.Sort(StringComparer.Ordinal);
        return new NativeToolchain
        {
            Platform = windows ? NativePlatform.Windows : NativePlatform.MacOS,
            CompilerPath = compilerPath,
            ArchiverPath = archiver,
            LinkerPath = linker,
            Identity = version.StdOut.Trim(),
            Environment = environment,
            AbiIdentity = Fingerprints.Strings(abi),
            CompileArguments = compile,
            LinkArguments = link,
            FingerprintPaths = paths.Distinct(PathComparer).ToArray(),
            RuntimeFiles = runtime,
            CompilerVersion = compilerVersion,
            TargetTriple = triple,
            CompilerDigest = compilerDigest,
            LinkerDigest = linkerDigest,
            OwnedRoots = owned,
            AbiFields = abiFields,
            RemovedEnvironment = reported.Removed
        };
    }

    /// <summary>
    /// Android NDK clang으로 교차 빌드하는 toolchain을 찾는다. abi는 "arm64" 또는 "x86_64",
    /// apiLevel은 최소 Android API(26 이상 39 이하)다. ndk가 없으면 ANDROID_NDK_HOME·ANDROID_NDK_ROOT,
    /// 그다음 SDK 기본 위치(Windows %LOCALAPPDATA%/Android/Sdk/ndk, macOS ~/Library/Android/sdk/ndk)의
    /// 가장 높은 버전을 쓴다. 증분 빌드 신원에는 NDK 버전(source.properties)과 compiler 버전을 넣는다.
    /// </summary>
    public static async Task<NativeToolchain> DiscoverAndroidAsync(string abi, int apiLevel, string? ndk = null,
        CancellationToken cancellationToken = default)
    {
        if (!OperatingSystem.IsWindows() && !OperatingSystem.IsMacOS())
        {
            throw new PlatformNotSupportedException("Android builds require a Windows or macOS host.");
        }

        string triple = abi switch
        {
            "arm64" => "aarch64-linux-android",
            "x86_64" => "x86_64-linux-android",
            _ => throw new ArgumentException("android-abi must be arm64 or x86_64.")
        };
        if (apiLevel < 26 || apiLevel > 39)
        {
            throw new ArgumentException("android-api must be between 26 and 39.");
        }

        string root = ndk ?? Environment.GetEnvironmentVariable("ANDROID_NDK_HOME")
            ?? Environment.GetEnvironmentVariable("ANDROID_NDK_ROOT") ?? FindDefaultNdk()
            ?? throw new FileNotFoundException("Android NDK not found. Pass --android-ndk or set ANDROID_NDK_HOME.");
        root = Path.GetFullPath(root);
        string properties = Path.Combine(root, "source.properties");
        if (!File.Exists(properties))
        {
            throw new FileNotFoundException("Not an Android NDK directory (source.properties missing).", properties);
        }

        string revision = File.ReadAllLines(properties).Select(line => line.Trim())
            .FirstOrDefault(line => line.StartsWith("Pkg.Revision", StringComparison.Ordinal))?.Split('=', 2)[1].Trim()
            ?? throw new InvalidOperationException("Cannot read Pkg.Revision from " + properties);
        string host = OperatingSystem.IsWindows() ? "windows-x86_64" : "darwin-x86_64";
        string bin = Path.Combine(root, "toolchains", "llvm", "prebuilt", host, "bin");
        string executable = OperatingSystem.IsWindows() ? ".exe" : "";
        string compiler = Path.Combine(bin, "clang++" + executable);
        string archiver = Path.Combine(bin, "llvm-ar" + executable);
        string lld = Path.Combine(bin, "ld.lld" + executable);
        foreach (string tool in new[] { compiler, archiver, lld })
        {
            if (!File.Exists(tool))
            {
                throw new FileNotFoundException("Android NDK tool is missing.", tool);
            }
        }

        // NDK는 Visual Studio 환경이 필요 없다. 허용 목록(Android Keyed + 호스트 Runtime)만 담은 새 환경을 쓴다.
        NativeEnvironmentResult restricted = NativeEnvironment.Restrict(Environment.GetEnvironmentVariables()
            .Cast<System.Collections.DictionaryEntry>().Select(entry => new KeyValuePair<string, string?>((string)entry.Key, (string?)entry.Value)),
            NativeEnvironment.AndroidKeyedVariables);
        var environment = restricted.Variables;
        IReadOnlyList<string> removed = restricted.Removed;
        (ProcessResult version, string[] config) = await ProbeVersionAsync(compiler, environment, cancellationToken);
        string compilerVersion = ParseCompilerVersion(version.StdOut);
        string target = "--target=" + triple + apiLevel;
        string[] compile = [.. config, target];
        string effective = await ProbeTargetTripleAsync(compiler, compile, false, environment, cancellationToken);
        var abiIdentity = new List<string> { "rocks-native-abi-v2", "Android", abi, "api=" + apiLevel, "ndk=" + revision };
        abiIdentity.AddRange(AbiVersionLines(version.StdOut, compilerVersion, effective));
        string[] abiFields = new[] { "compiler=" + compilerVersion, "ndk=" + revision }.Order(StringComparer.Ordinal).ToArray();
        return new NativeToolchain
        {
            Platform = NativePlatform.Android,
            CompilerPath = compiler,
            ArchiverPath = archiver,
            LinkerPath = null,
            Identity = version.StdOut.Trim() + "\n" + target + "\nndk=" + revision,
            Environment = environment,
            AbiIdentity = Fingerprints.Strings(abiIdentity),
            // 엔진은 STL을 쓰지 않지만 compiler runtime(예: 정적 지역 변수 guard)을 위해 libc++를 정적으로 묶는다.
            CompileArguments = compile,
            LinkArguments = [.. config, target, "-static-libstdc++"],
            FingerprintPaths = [properties],
            RuntimeFiles = [],
            CompilerVersion = compilerVersion,
            TargetTriple = effective,
            CompilerDigest = Hashing.Sha256File(compiler),
            LinkerDigest = Hashing.Sha256File(lld),
            OwnedRoots = new Dictionary<string, string>(StringComparer.Ordinal) { ["ndk"] = root },
            AbiFields = abiFields,
            RemovedEnvironment = removed
        };
    }

    /// <summary>
    /// 빌드 환경에서 지운 컴파일러 동작 변수를 알리는 한 줄이다(없으면 null). 빌드 CLI가 discovery 직후 출력한다.
    /// 예: "Build environment: removed CL, LINK (compiler/linker behavior variables are not inherited)".
    /// </summary>
    public static string? EnvironmentNotice(NativeToolchain toolchain)
    {
        ArgumentNullException.ThrowIfNull(toolchain);
        return toolchain.RemovedEnvironment.Count == 0 ? null
            : "Build environment: removed " + string.Join(", ", toolchain.RemovedEnvironment) +
              " (compiler/linker behavior variables are not inherited)";
    }

    /// <summary>
    /// `--version` 출력에서 컴파일러 식별·버전(<see cref="NativeToolchain.CompilerVersion"/>)을 만든다. 처음 비어 있지 않은 줄을
    /// 쓰고 <c>clang version</c>이 없으면 InvalidOperationException이다. 그 줄의 괄호 묶음 중 경로나 URL(<c>/</c>·<c>\</c>·<c>:</c>
    /// 포함)을 담은 것은 안의 7–40자 hex 토큰만 남기고(없으면 묶음째) 지운다 — 자가 빌드 LLVM의 로컬 저장소 경로를 없애고
    /// commit hash만 남긴다. 경로가 없는 괄호(NDK의 <c>Android (…, based on r…)</c>)는 그대로 둔다.
    /// </summary>
    internal static string ParseCompilerVersion(string versionOutput)
    {
        string line = versionOutput.Replace("\r", "", StringComparison.Ordinal).Split('\n')
            .Select(value => value.Trim()).FirstOrDefault(value => value.Length != 0) ?? "";
        if (!line.Contains("clang version", StringComparison.Ordinal))
        {
            throw new InvalidOperationException("Cannot identify the compiler version: the first line of --version does not contain 'clang version': " + line);
        }

        var result = new StringBuilder();
        int index = 0;
        while (index < line.Length)
        {
            if (line[index] != '(')
            {
                result.Append(line[index]);
                ++index;
                continue;
            }

            int depth = 0;
            int end = index;
            for (; end < line.Length; ++end)
            {
                if (line[end] == '(')
                {
                    ++depth;
                }
                else if (line[end] == ')' && --depth == 0)
                {
                    break;
                }
            }

            if (end >= line.Length)
            {
                // 닫히지 않은 괄호는 묶음이 아니므로 나머지를 그대로 둔다(공식·NDK·자가 빌드 형태는 모두 닫힌다).
                result.Append(line, index, line.Length - index);
                break;
            }

            string content = line.Substring(index + 1, end - index - 1);
            if (content.IndexOfAny(['/', '\\', ':']) < 0)
            {
                result.Append(line, index, end - index + 1);
            }
            else
            {
                string[] hashes = content.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
                    .Where(token => token.Length is >= 7 and <= 40 && token.All(char.IsAsciiHexDigit)).ToArray();
                if (hashes.Length != 0)
                {
                    result.Append('(').Append(string.Join(' ', hashes)).Append(')');
                }
            }

            index = end + 1;
        }

        string normalized = string.Join(' ', result.ToString().Split(' ', StringSplitOptions.RemoveEmptyEntries));
        return normalized;
    }

    /// <summary>
    /// <c>-print-effective-triple</c> 출력을 검사한다. CR은 지우고 마지막 줄바꿈 하나를 뗀 뒤 빈 값·여러 줄·
    /// <c>[A-Za-z0-9._+-]</c> 밖 문자는 InvalidOperationException이다.
    /// </summary>
    internal static string ParseTargetTriple(string output)
    {
        string text = output.Replace("\r", "", StringComparison.Ordinal);
        if (text.EndsWith('\n'))
        {
            text = text[..^1];
        }

        if (text.Length == 0 || text.Any(value => !char.IsAsciiLetterOrDigit(value) && value is not ('.' or '_' or '+' or '-')))
        {
            throw new InvalidOperationException("Cannot identify the effective target triple: unexpected -print-effective-triple output '" +
                output.Replace("\n", "\\n", StringComparison.Ordinal) + "'.");
        }

        return text;
    }

    // `--no-default-config --version`이 통과하면 그 인자를 쓴다. 받지 않는 컴파일러는 인자 없이 다시 묻고, 그때 기본 config 파일을
    // 읽고 있으면(Configuration file: 줄) 끌 수 없으므로 discovery 오류다.
    private static async Task<(ProcessResult Version, string[] Config)> ProbeVersionAsync(string compiler,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken)
    {
        var probed = await ProcessRunner.RunAsync(new ProcessSpec(compiler, [NoDefaultConfig, "--version"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        bool accepted = probed.ExitCode == 0 && !probed.StdErr.Contains(NoDefaultConfig, StringComparison.Ordinal);
        ProcessResult version = accepted ? probed : await ProcessRunner.RunAsync(new ProcessSpec(compiler, ["--version"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (version.ExitCode != 0 || !version.StdOut.Contains("clang", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("A working Clang compiler is required.");
        }

        string? configuration = version.StdOut.Replace("\r", "", StringComparison.Ordinal).Split('\n')
            .FirstOrDefault(line => line.TrimStart().StartsWith("Configuration file:", StringComparison.OrdinalIgnoreCase));
        if (configuration is not null)
        {
            throw new InvalidOperationException(accepted
                ? $"The compiler still loads a configuration file with {NoDefaultConfig}: {configuration.Trim()}"
                : $"The compiler loads a default configuration file but does not accept {NoDefaultConfig}: {configuration.Trim()}");
        }

        return (version, accepted ? [NoDefaultConfig] : []);
    }

    private static async Task<string> ProbeTargetTripleAsync(string compiler, IReadOnlyList<string> compileArguments, bool clangCl,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken)
    {
        var result = await ProcessRunner.RunAsync(new ProcessSpec(compiler,
            [.. compileArguments, clangCl ? "/clang:-print-effective-triple" : "-print-effective-triple"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException("Cannot identify the effective target triple: " + result.StdErr);
        }

        return ParseTargetTriple(result.StdOut);
    }

    // AbiIdentity에 넣는 `--version` 줄이다. 첫 줄은 정규화된 CompilerVersion, InstalledDir:·Target: 줄은 빼고(Target:은 Apple
    // clang에서 호스트 커널 버전이다), 대상은 MSVC 환경 버전 접미를 지운 effective triple로 넣는다.
    private static IEnumerable<string> AbiVersionLines(string versionOutput, string compilerVersion, string triple)
    {
        yield return compilerVersion;
        foreach (string line in versionOutput.Replace("\r", "", StringComparison.Ordinal).Split('\n').Select(value => value.Trim())
            .Where(value => value.Length != 0).Skip(1))
        {
            if (!line.StartsWith("InstalledDir:", StringComparison.OrdinalIgnoreCase) && !line.StartsWith("Target:", StringComparison.OrdinalIgnoreCase))
            {
                yield return line;
            }
        }

        yield return "target=" + ToolchainFingerprint.PortableTarget(triple);
    }

    // macOS 컴파일러·링커 바이너리 digest다. /usr/bin의 clang·clang++·cc·c++는 xcrun shim이라 Xcode 버전을 구별하지 못하므로 그때만
    // xcrun --find clang++로 찾은 실제 컴파일러를 해시하고, 그 밖에는 주어진 경로(링크를 따라간 내용)를 해시한다. 링크는 ld와
    // libLTO.dylib의 digest를 이은 텍스트를 해시한다. ld는 clang driver 탐색 순서대로 컴파일러 옆(호출 경로, 그다음 링크를 따라간
    // 실제 경로)에 있으면 그것, 없으면 xcrun --find ld다. libLTO.dylib는 실제 컴파일러 기준 ../lib의 것이다.
    private static async Task<(string Compiler, string Linker)> MacBinaryDigestsAsync(string compilerPath,
        string xcrun, IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken)
    {
        async Task<string> FindAsync(string tool)
        {
            var found = await ProcessRunner.RunAsync(new ProcessSpec(xcrun, ["--find", tool],
                Directory.GetCurrentDirectory(), environment), cancellationToken);
            string path = found.StdOut.Trim();
            if (found.ExitCode != 0 || !File.Exists(path))
            {
                throw new InvalidOperationException($"Cannot find {tool} with xcrun --find: {found.StdErr}");
            }

            return Path.GetFullPath(path);
        }

        string compiler = IsXcrunShim(compilerPath) ? await FindAsync("clang++") : compilerPath;
        string resolved = new FileInfo(compiler).ResolveLinkTarget(true)?.FullName ?? compiler;
        string? ld = new[] { Path.GetDirectoryName(compiler)!, Path.GetDirectoryName(resolved)! }
            .Select(directory => Path.Combine(directory, "ld")).FirstOrDefault(File.Exists);
        ld ??= await FindAsync("ld");
        string lto = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(resolved)!, "..", "lib", "libLTO.dylib"));
        if (!File.Exists(lto))
        {
            throw new InvalidOperationException("Cannot find libLTO.dylib for the compiler: " + lto);
        }

        return (Hashing.Sha256File(compiler), Hashing.Sha256Text(Hashing.Sha256File(ld) + "\n" + Hashing.Sha256File(lto) + "\n"));
    }

    // /usr/bin의 컴파일러 진입점은 Xcode가 설치하는 xcrun shim이다(실제 컴파일러는 선택된 Xcode toolchain 안에 있다).
    internal static bool IsXcrunShim(string compilerPath) =>
        string.Equals(Path.GetDirectoryName(Path.GetFullPath(compilerPath)), "/usr/bin", StringComparison.Ordinal) &&
        Path.GetFileName(compilerPath) is "clang" or "clang++" or "cc" or "c++";

    private static string? FindDefaultNdk()
    {
        string sdk = OperatingSystem.IsWindows()
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk")
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Android", "sdk");
        sdk = Environment.GetEnvironmentVariable("ANDROID_HOME") ?? Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT") ?? sdk;
        string ndkRoot = Path.Combine(sdk, "ndk");
        if (!Directory.Exists(ndkRoot))
        {
            return null;
        }

        return Directory.EnumerateDirectories(ndkRoot)
            .Select(path => (path, version: Version.TryParse(Path.GetFileName(path), out var parsed) ? parsed : null))
            .Where(item => item.version != null && File.Exists(Path.Combine(item.path, "source.properties")))
            .OrderByDescending(item => item.version).Select(item => item.path).FirstOrDefault();
    }

    internal static StringComparer PathComparer => OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private static string? FindBeside(string directory, string name)
    {
        string candidate = Path.Combine(directory, name);
        return File.Exists(candidate) ? candidate : null;
    }

    private static string FindFirst(IReadOnlyDictionary<string, string?> environment, string[] candidates)
    {
        foreach (string candidate in candidates)
        {
            try
            {
                return NativeEnvironment.FindExecutable(candidate, environment);
            }
            catch (Exception error) when (error is FileNotFoundException or InvalidOperationException) { }
        }

        throw new FileNotFoundException($"Native tool not found: {string.Join(", ", candidates)}");
    }
}
