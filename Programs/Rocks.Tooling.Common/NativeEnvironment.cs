using System.Collections;
using System.Text;

namespace Rocks.Tooling.Common;

/// <summary>
/// 빌드 환경을 만든 결과다. Variables는 자식 프로세스에 그대로 넘길 환경(허용 목록의 정규 철자만)이고, Removed는 허용 목록 밖이라
/// 빠진 변수 중 컴파일러·링커 동작에 영향을 주는 것으로 알려진 이름이다(<see cref="NativeEnvironment.CompilerBehaviorVariables"/>
/// 철자, ordinal 정렬 — 빌드 출력에 알린다).
/// </summary>
public sealed record NativeEnvironmentResult(Dictionary<string, string?> Variables, IReadOnlyList<string> Removed);

/// <summary>
/// 네이티브 빌드 환경이다(Docs/Design/ReflectionFoundations.ko.md 8.1절 빌드 환경 위생). 프로세스 환경을 통째로 물려주지 않고 허용
/// 목록의 변수만 담은 새 환경을 만든다. 허용 목록은 둘로 나뉜다.
/// <list type="bullet">
/// <item>Keyed: 컴파일 결과에 영향을 줄 수 있어 증분 key(<c>Fingerprints.Toolchain</c>)에 들어가는 변수(SDK·도구 위치·버전,
/// SOURCE_DATE_EPOCH 등).</item>
/// <item>Runtime: 자식 프로세스 실행에만 필요하고 key에서 빼는 변수(PATH·TEMP·시스템 폴더 등). 도구는 절대 경로와 바이너리
/// digest로 고정되므로 PATH가 달라도 결과가 같다.</item>
/// </list>
/// 그 밖의 변수(<c>CL</c>·<c>LINK</c>·<c>CCC_OVERRIDE_OPTIONS</c> 같은 컴파일러 동작 변수, 실행마다 바뀌는 세션 ID 등)는 넘기지
/// 않는다. 이름은 목록의 정규 철자로 담는다(Windows의 Path·PATH 차이를 없앤다).
/// </summary>
public static class NativeEnvironment
{
    /// <summary>
    /// 허용 목록 밖에서 컴파일러·링커 동작을 몰래 바꾸는 것으로 알려진 변수다. CL·_CL_는 clang-cl, LINK·_LINK_는 lld-link·link.exe,
    /// CCC_OVERRIDE_OPTIONS는 clang driver 인자를 바꾸고(실측: <c>CL=/fp:fast</c>가 clang-cl에 fast-math를 켠다), 나머지는
    /// include·라이브러리·도구 검색 경로를 더한다. 허용 목록 방식이라 언제나 빠지며, 프로세스 환경에 있었으면 이름을 알린다.
    /// </summary>
    public static readonly IReadOnlyList<string> CompilerBehaviorVariables =
    [
        "CL", "_CL_", "LINK", "_LINK_", "CCC_OVERRIDE_OPTIONS", "COMPILER_PATH", "CPATH", "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "LIBRARY_PATH"
    ];

    /// <summary>Windows 대상의 Keyed 변수다(Visual Studio·Windows SDK 환경과 SOURCE_DATE_EPOCH).</summary>
    public static readonly IReadOnlyList<string> WindowsKeyedVariables =
    [
        "INCLUDE", "EXTERNAL_INCLUDE", "LIB", "LIBPATH", "VCToolsInstallDir", "VCToolsVersion", "VCINSTALLDIR", "VSINSTALLDIR",
        "WindowsSdkDir", "WindowsSDKVersion", "WindowsSDKLibVersion", "UniversalCRTSdkDir", "UCRTVersion", "SOURCE_DATE_EPOCH"
    ];

    /// <summary>
    /// Windows 호스트의 Runtime 변수다. ROCKS_CLANG_FORMAT(MetaCompiler·ToolTests의 고정 formatter 위치)과 APPDATA(dotnet·NuGet 사용자
    /// 설정 — 게임 규칙 빌드)는 같은 환경으로 실행하는 빌드 도구를 위해 더한다.
    /// </summary>
    public static readonly IReadOnlyList<string> WindowsRuntimeVariables =
    [
        "PATH", "PATHEXT", "SystemRoot", "SystemDrive", "windir", "ComSpec", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
        "ProgramData", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
        "ROCKS_CLANG_FORMAT"
    ];

    /// <summary>macOS 대상의 Keyed 변수다(Xcode 선택·SDK·배포 대상·결정적 archive·SOURCE_DATE_EPOCH).</summary>
    public static readonly IReadOnlyList<string> MacKeyedVariables =
    [
        "DEVELOPER_DIR", "SDKROOT", "TOOLCHAINS", "MACOSX_DEPLOYMENT_TARGET", "ZERO_AR_DATE", "SOURCE_DATE_EPOCH"
    ];

    /// <summary>macOS 호스트의 Runtime 변수다(ROCKS_CLANG_FORMAT은 Windows와 같은 이유로 더한다).</summary>
    public static readonly IReadOnlyList<string> MacRuntimeVariables =
    [
        "PATH", "HOME", "TMPDIR", "USER", "LANG", "LC_ALL", "ROCKS_CLANG_FORMAT"
    ];

    /// <summary>Android 대상의 Keyed 변수다. Runtime은 호스트 OS 목록을 쓴다.</summary>
    public static readonly IReadOnlyList<string> AndroidKeyedVariables = ["SOURCE_DATE_EPOCH"];

    /// <summary>이 호스트의 Runtime 변수 목록이다.</summary>
    public static IReadOnlyList<string> HostRuntimeVariables => OperatingSystem.IsWindows() ? WindowsRuntimeVariables : MacRuntimeVariables;

    /// <summary>이 호스트를 대상으로 하는 빌드(Windows 또는 macOS)의 Keyed 변수 목록이다.</summary>
    public static IReadOnlyList<string> HostKeyedVariables => OperatingSystem.IsWindows() ? WindowsKeyedVariables : MacKeyedVariables;

    /// <summary>
    /// 호스트 대상 빌드 환경을 만든다(Windows는 Visual Studio 환경을 더한 뒤 고른다). 허용 목록(<see cref="HostKeyedVariables"/>·
    /// <see cref="HostRuntimeVariables"/>) 밖의 변수는 넘기지 않는다. 빠진 컴파일러 동작 변수 이름이 필요하면
    /// <see cref="CreateReportedAsync"/>를 쓴다.
    /// </summary>
    public static async Task<Dictionary<string, string?>> CreateAsync(string? compiler = null,
        CancellationToken cancellationToken = default) =>
        (await CreateReportedAsync(compiler, cancellationToken)).Variables;

    /// <summary>
    /// <see cref="CreateAsync"/>와 같은 환경을 만들고, 허용 목록 밖이라 빠진 컴파일러 동작 변수 이름을 함께 돌려준다(toolchain
    /// discovery의 <c>NativeToolchain.RemovedEnvironment</c>). compiler를 주면(Windows) 그 컴파일러의 ASan runtime DLL 디렉터리를
    /// PATH 앞에 더한다. 그 probe가 실패하면 InvalidOperationException이다.
    /// </summary>
    public static async Task<NativeEnvironmentResult> CreateReportedAsync(string? compiler = null,
        CancellationToken cancellationToken = default)
    {
        var source = new Dictionary<string, string?>(OperatingSystem.IsWindows()
            ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        foreach (DictionaryEntry item in System.Environment.GetEnvironmentVariables())
        {
            source[(string)item.Key] = (string?)item.Value;
        }

        if (OperatingSystem.IsWindows() && !source.ContainsKey("VCToolsInstallDir"))
        {
            var installer = Path.Combine(System.Environment.GetFolderPath(
                System.Environment.SpecialFolder.ProgramFilesX86), "Microsoft Visual Studio", "Installer", "vswhere.exe");
            var result = await ProcessRunner.RunAsync(new(installer,
                ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"], Directory.GetCurrentDirectory(), source), cancellationToken);
            var installation = result.StdOut.Trim();
            if (result.ExitCode != 0 || installation.Length == 0)
            {
                throw new InvalidOperationException("Visual Studio C++ toolchain not found.");
            }

            var script = Path.Combine(installation, "Common7", "Tools", "VsDevCmd.bat");
            if (script.IndexOfAny(['\"', '\r', '\n', '%']) >= 0)
            {
                throw new InvalidOperationException("Unsupported Visual Studio installation path.");
            }
            // cmd는 MS 제공 환경 진입점에만 사용한다. 사용자 compiler/source 인자는 shell을 통과하지 않는다.
            result = await ProcessRunner.RunAsync(new("cmd.exe",
                ["/d", "/u", "/c", "call", script, "-no_logo", "-arch=x64", "-host_arch=x64", ">nul", "&&", "set"],
                Directory.GetCurrentDirectory(), source, StandardOutputEncoding: Encoding.Unicode, StandardErrorEncoding: Encoding.Unicode), cancellationToken);
            if (result.ExitCode != 0)
            {
                throw new InvalidOperationException("Visual Studio environment initialization failed: " + result.StdErr);
            }

            foreach (var line in result.StdOut.Split('\n'))
            {
                var separator = line.IndexOf('=');
                if (separator > 0)
                {
                    source[line[..separator]] = line[(separator + 1)..].TrimEnd('\r');
                }
            }
        }

        // Visual Studio 환경 스크립트의 출력까지 합친 뒤 고른다. 아래 probe도 고른 환경으로 실행한다.
        NativeEnvironmentResult restricted = Restrict(source, HostKeyedVariables);
        Dictionary<string, string?> environment = restricted.Variables;
        if (compiler is not null && OperatingSystem.IsWindows())
        {
            // 기본 config 파일을 끄는 인자는 받는 컴파일러에만 쓴다(discovery와 같은 조건 — 받지 않으면 인자 없이 다시 묻는다).
            var result = await ProcessRunner.RunAsync(new(compiler, ["--no-default-config", "/clang:-print-resource-dir"],
                Directory.GetCurrentDirectory(), environment), cancellationToken);
            if (result.ExitCode != 0 || result.StdErr.Contains("--no-default-config", StringComparison.Ordinal))
            {
                result = await ProcessRunner.RunAsync(new(compiler, ["/clang:-print-resource-dir"],
                    Directory.GetCurrentDirectory(), environment), cancellationToken);
            }

            if (result.ExitCode != 0 || result.StdOut.Trim().Length == 0)
            {
                throw new InvalidOperationException($"Cannot identify the compiler resource directory ({compiler}): {result.StdErr}");
            }

            var resource = result.StdOut.Trim();
            var directories = new[] { "lib/x86_64-unknown-windows-msvc", "lib/x86_64-pc-windows-msvc", "lib/windows" }
                .Select(relative => Path.Combine(resource, relative)).Where(Directory.Exists);
            environment["PATH"] = string.Join(Path.PathSeparator, directories.Append(environment.GetValueOrDefault("PATH") ?? ""));
        }

        return restricted;
    }

    /// <summary>
    /// source에서 keyed 목록과 이 호스트의 Runtime 목록(<see cref="HostRuntimeVariables"/>)에 있는 변수만 골라 정규 철자로 담은 새
    /// 환경을 만든다. 이름 대조는 Windows 호스트에서 대소문자를 무시한다. Removed는 source에 있던
    /// <see cref="CompilerBehaviorVariables"/>다. Android NDK discovery는 keyed로 <see cref="AndroidKeyedVariables"/>를 넘긴다.
    /// </summary>
    public static NativeEnvironmentResult Restrict(IEnumerable<KeyValuePair<string, string?>> source, IReadOnlyList<string> keyed)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(keyed);
        StringComparer comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        var values = new Dictionary<string, string?>(comparer);
        foreach (var pair in source)
        {
            values[pair.Key] = pair.Value;
        }

        var environment = new Dictionary<string, string?>(comparer);
        foreach (string name in keyed.Concat(HostRuntimeVariables))
        {
            if (values.TryGetValue(name, out string? value) && value is not null && !environment.ContainsKey(name))
            {
                environment.Add(name, value);
            }
        }

        string[] removed = CompilerBehaviorVariables.Where(values.ContainsKey).Order(StringComparer.Ordinal).ToArray();
        return new NativeEnvironmentResult(environment, removed);
    }

    public static string FindExecutable(string name, IReadOnlyDictionary<string, string?> environment)
    {
        if (Path.IsPathRooted(name) || name.Contains(Path.DirectorySeparatorChar) || name.Contains(Path.AltDirectorySeparatorChar))
        {
            if (File.Exists(name))
            {
                return Path.GetFullPath(name);
            }

            throw new FileNotFoundException("Executable not found.", name);
        }

        var path = environment.FirstOrDefault(pair => string.Equals(pair.Key, "PATH",
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value ?? "";
        var suffixes = OperatingSystem.IsWindows() && Path.GetExtension(name).Length == 0
            ? new[] { ".exe", ".cmd", ".bat", "" } : new[] { "" };
        foreach (var directory in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            foreach (var suffix in suffixes)
            {
                var candidate = Path.Combine(directory.Trim('"'), name + suffix);
                if (File.Exists(candidate))
                {
                    return Path.GetFullPath(candidate);
                }
            }
        }

        throw new FileNotFoundException("Executable not found on PATH.", name);
    }
}
