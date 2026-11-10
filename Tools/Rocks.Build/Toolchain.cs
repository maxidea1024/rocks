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
        string archiver = FindFirst(environment, windows ? ["llvm-lib", "lib.exe"] : ["llvm-ar", "/usr/bin/ar"]);
        string? linker = windows ? FindFirst(environment, ["lld-link", "link.exe"]) : null;
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

    internal static StringComparer PathComparer => OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

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
