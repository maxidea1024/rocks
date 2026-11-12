using Rocks.Tooling.Common;
using System.Runtime.InteropServices;

namespace Rocks.Build;

public static class NativeToolchains
{
    public static async Task<NativeToolchain> DiscoverAsync(string? compiler = null,
        CancellationToken cancellationToken = default)
    {
        var environment = await NativeEnvironment.CreateAsync(compiler, cancellationToken);
        bool windows = OperatingSystem.IsWindows();
        if (!windows && !OperatingSystem.IsMacOS())
            throw new PlatformNotSupportedException("Only Windows and macOS native hosts are supported.");
        string compilerPath = NativeEnvironment.FindExecutable(compiler ?? (windows ? "clang-cl" : "clang++"), environment);
        var version = await ProcessRunner.RunAsync(new ProcessSpec(compilerPath, ["--version"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (version.ExitCode != 0 || !version.StdOut.Contains("clang", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("A working Clang compiler is required.");
        // archiver·linker는 compiler와 같은 LLVM 설치에서 먼저 찾는다. Release의 ThinLTO bitcode는 같은 LLVM
        // 버전의 도구만 확실히 읽으며, clang-cl의 -fuse-ld=lld도 자기 폴더의 lld-link를 쓴다. macOS에서
        // compiler 옆에 llvm-ar가 없으면(Apple clang) PATH의 다른 llvm-ar 대신 Xcode의 ar(libLTO)를 쓴다.
        string compilerDirectory = Path.GetDirectoryName(compilerPath)!;
        string archiver = FindBeside(compilerDirectory, windows ? "llvm-lib.exe" : "llvm-ar")
            ?? (windows ? FindFirst(environment, ["llvm-lib", "lib.exe"]) : "/usr/bin/ar");
        string? linker = windows ? FindBeside(compilerDirectory, "lld-link.exe") ?? FindFirst(environment, ["lld-link", "link.exe"]) : null;
        var paths = new List<string>();
        var runtime = new List<string>();
        var compile = new List<string>();
        var link = new List<string>();
        var abi = new List<string> { "rocks-native-abi-v1", windows ? "Windows" : "MacOS", RuntimeInformation.ProcessArchitecture.ToString() };
        abi.AddRange(version.StdOut.Split('\n').Select(value => value.Trim())
            .Where(value => value.Length != 0 && !value.StartsWith("InstalledDir:", StringComparison.OrdinalIgnoreCase)));
        string EnvironmentValue(string name) => environment.FirstOrDefault(pair => pair.Key.Equals(name,
            windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value ?? "";
        var resource = await ProcessRunner.RunAsync(new ProcessSpec(compilerPath,
            [windows ? "/clang:-print-resource-dir" : "-print-resource-dir"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (resource.ExitCode != 0)
            throw new InvalidOperationException("Cannot identify the compiler resource directory.");
        string resourcePath = resource.StdOut.Trim();
        if (Directory.Exists(resourcePath))
        {
            paths.Add(resourcePath);
            if (windows)
            {
                // Windows host contract는 x64다. resource tree 전체를 열거하면 i686 DLL도 같은
                // 파일명으로 수집되어 잘못 배포되므로 compiler가 사용하는 x64 디렉터리만 고른다.
                foreach (string relative in new[] { "lib/x86_64-unknown-windows-msvc", "lib/x86_64-pc-windows-msvc", "lib/windows" })
                {
                    string directory = Path.Combine(resourcePath, relative);
                    if (!Directory.Exists(directory)) continue;
                    string[] files = Directory.EnumerateFiles(directory, "clang_rt.asan_dynamic*.dll").ToArray();
                    if (files.Length == 0) continue;
                    runtime.AddRange(files.Where(file => !Path.GetFileName(file).Contains("i386", StringComparison.OrdinalIgnoreCase)));
                    break;
                }
            }
        }
        if (windows)
        {
            abi.Add("WindowsSDKVersion=" + EnvironmentValue("WindowsSDKVersion").TrimEnd('/', '\\'));
            abi.Add("VCToolsVersion=" + EnvironmentValue("VCToolsVersion").TrimEnd('/', '\\'));
            // SDK 환경의 실제 include/lib 내용도 key에 넣어 같은 경로에서 교체된 SDK를 구별한다.
            foreach (string variable in new[] { "INCLUDE", "LIB" })
            {
                string? value = environment.FirstOrDefault(pair => pair.Key.Equals(variable, StringComparison.OrdinalIgnoreCase)).Value;
                foreach (string directory in (value ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                    if (Directory.Exists(directory)) paths.Add(directory);
            }
        }
        else
        {
            string xcrun = NativeEnvironment.FindExecutable("xcrun", environment);
            var sdk = await ProcessRunner.RunAsync(new ProcessSpec(xcrun, ["--show-sdk-path"],
                Directory.GetCurrentDirectory(), environment), cancellationToken);
            if (sdk.ExitCode != 0 || !Directory.Exists(sdk.StdOut.Trim()))
                throw new InvalidOperationException("Cannot identify the macOS SDK.");
            string sdkPath = sdk.StdOut.Trim();
            foreach (string option in new[] { "--show-sdk-version", "--show-sdk-build-version" })
            {
                var sdkVersion = await ProcessRunner.RunAsync(new ProcessSpec(xcrun, [option],
                    Directory.GetCurrentDirectory(), environment), cancellationToken);
                if (sdkVersion.ExitCode != 0 || string.IsNullOrWhiteSpace(sdkVersion.StdOut))
                    throw new InvalidOperationException("Cannot identify macOS SDK ABI: " + option);
                abi.Add(option + "=" + sdkVersion.StdOut.Trim());
            }
            string deployment = EnvironmentValue("MACOSX_DEPLOYMENT_TARGET");
            if (deployment.Length == 0) deployment = "13.0";
            if (!Version.TryParse(deployment, out var minimum) || minimum.Major < 13 ||
                deployment.Any(value => !char.IsAsciiDigit(value) && value != '.'))
                throw new ArgumentException("MACOSX_DEPLOYMENT_TARGET must be a numeric version >= 13.0.");
            environment["MACOSX_DEPLOYMENT_TARGET"] = deployment;
            compile.Add("-mmacosx-version-min=" + deployment);
            link.Add("-mmacosx-version-min=" + deployment);
            abi.Add("deployment=" + deployment);
            compile.AddRange(["-isysroot", sdkPath]);
            link.AddRange(["-isysroot", sdkPath]);
            paths.Add(sdkPath);
            // Apple ar의 deterministic archive 모드. 환경 값도 action key에 포함된다.
            environment["ZERO_AR_DATE"] = "1";
        }
        return new NativeToolchain
        {
            Platform = windows ? NativePlatform.Windows : NativePlatform.MacOS,
            CompilerPath = compilerPath, ArchiverPath = archiver, LinkerPath = linker,
            Identity = version.StdOut.Trim(), Environment = environment,
            AbiIdentity = Fingerprints.Strings(abi),
            CompileArguments = compile, LinkArguments = link,
            FingerprintPaths = paths.Distinct(PathComparer).ToArray(), RuntimeFiles = runtime
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
            throw new PlatformNotSupportedException("Android builds require a Windows or macOS host.");
        string triple = abi switch
        {
            "arm64" => "aarch64-linux-android",
            "x86_64" => "x86_64-linux-android",
            _ => throw new ArgumentException("android-abi must be arm64 or x86_64.")
        };
        if (apiLevel < 26 || apiLevel > 39)
            throw new ArgumentException("android-api must be between 26 and 39.");
        string root = ndk ?? Environment.GetEnvironmentVariable("ANDROID_NDK_HOME")
            ?? Environment.GetEnvironmentVariable("ANDROID_NDK_ROOT") ?? FindDefaultNdk()
            ?? throw new FileNotFoundException("Android NDK not found. Pass --android-ndk or set ANDROID_NDK_HOME.");
        root = Path.GetFullPath(root);
        string properties = Path.Combine(root, "source.properties");
        if (!File.Exists(properties))
            throw new FileNotFoundException("Not an Android NDK directory (source.properties missing).", properties);
        string revision = File.ReadAllLines(properties).Select(line => line.Trim())
            .FirstOrDefault(line => line.StartsWith("Pkg.Revision", StringComparison.Ordinal))?.Split('=', 2)[1].Trim()
            ?? throw new InvalidOperationException("Cannot read Pkg.Revision from " + properties);
        string host = OperatingSystem.IsWindows() ? "windows-x86_64" : "darwin-x86_64";
        string bin = Path.Combine(root, "toolchains", "llvm", "prebuilt", host, "bin");
        string executable = OperatingSystem.IsWindows() ? ".exe" : "";
        string compiler = Path.Combine(bin, "clang++" + executable);
        string archiver = Path.Combine(bin, "llvm-ar" + executable);
        foreach (string tool in new[] { compiler, archiver })
            if (!File.Exists(tool)) throw new FileNotFoundException("Android NDK tool is missing.", tool);
        var environment = Environment.GetEnvironmentVariables().Cast<System.Collections.DictionaryEntry>()
            .ToDictionary(entry => (string)entry.Key, entry => (string?)entry.Value,
                OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        var version = await ProcessRunner.RunAsync(new ProcessSpec(compiler, ["--version"],
            Directory.GetCurrentDirectory(), environment), cancellationToken);
        if (version.ExitCode != 0 || !version.StdOut.Contains("clang", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The Android NDK clang does not run: " + compiler);
        string target = "--target=" + triple + apiLevel;
        var abiIdentity = new List<string> { "rocks-native-abi-v1", "Android", abi, "api=" + apiLevel, "ndk=" + revision };
        abiIdentity.AddRange(version.StdOut.Split('\n').Select(value => value.Trim())
            .Where(value => value.Length != 0 && !value.StartsWith("InstalledDir:", StringComparison.OrdinalIgnoreCase)));
        return new NativeToolchain
        {
            Platform = NativePlatform.Android,
            CompilerPath = compiler, ArchiverPath = archiver, LinkerPath = null,
            Identity = version.StdOut.Trim() + "\n" + target + "\nndk=" + revision, Environment = environment,
            AbiIdentity = Fingerprints.Strings(abiIdentity),
            // 엔진은 STL을 쓰지 않지만 compiler runtime(예: 정적 지역 변수 guard)을 위해 libc++를 정적으로 묶는다.
            CompileArguments = [target], LinkArguments = [target, "-static-libstdc++"],
            FingerprintPaths = [properties], RuntimeFiles = []
        };
    }

    private static string? FindDefaultNdk()
    {
        string sdk = OperatingSystem.IsWindows()
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk")
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Android", "sdk");
        sdk = Environment.GetEnvironmentVariable("ANDROID_HOME") ?? Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT") ?? sdk;
        string ndkRoot = Path.Combine(sdk, "ndk");
        if (!Directory.Exists(ndkRoot)) return null;
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
            try { return NativeEnvironment.FindExecutable(candidate, environment); }
            catch (Exception error) when (error is FileNotFoundException or InvalidOperationException) { }
        }
        throw new FileNotFoundException($"Native tool not found: {string.Join(", ", candidates)}");
    }
}
