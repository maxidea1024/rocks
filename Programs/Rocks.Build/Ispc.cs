using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 찾은 ISPC 컴파일러다(Docs/Design/BuildTargets 3.12절 BD45·BD49). Version은 고정 버전(<c>Build/Toolchain.json</c> <c>ispc.version</c>)이고
/// 찾을 때 <c>--version</c> 출력으로 확인한 값이다. Digest는 실행 파일 내용의 SHA-256(소문자 hex 64자)이다 — 찾을 때 한 번만 재고 Ispc
/// action의 Identity에 넣는다(같은 버전 문자열을 내는 다른 빌드를 구별한다).
/// </summary>
public sealed record IspcTool(string Path, string Version, string Digest);

/// <summary>
/// ISPC 원본 컴파일의 고정 규칙이다(Docs/Design/BuildTargets 3.12절). 출력 byte가 호스트 CPU와 무관하도록 아키텍처당 타깃 하나이고
/// 곱셈·덧셈 합치기(FMA)와 fast-math를 끈다. 모듈 인자로 바꿀 수 없다.
/// </summary>
public static class IspcTools
{
    /// <summary>ISPC 실행 파일 경로를 주는 환경 변수다(<c>Build/InstallIspc.ps1</c>·Setup이 설정한다).</summary>
    public const string EnvironmentVariable = "ROCKS_ISPC";

    /// <summary>ISPC 원본 확장자다.</summary>
    public const string SourceExtension = ".ispc";

    /// <summary>생성 헤더 이름의 꼬리다(<c>kernel.ispc</c> → <c>kernel_ispc.h</c>).</summary>
    public const string HeaderSuffix = "_ispc.h";

    /// <summary>모듈 소비자에게 ISPC 신원을 알리는 정의 이름이다(BD50 — 인코더가 너겟 키에 쓴다).</summary>
    public const string IdentityMacro = "ROCKS_ISPC_IDENTITY";

    /// <summary>
    /// 대상 아키텍처의 ISPC 타깃과 <c>--arch</c> 값이다. X64는 SSE4(i32x4), Arm64는 NEON(i32x4) 하나다. 다중 타깃 dispatch는 쓰지 않는다
    /// (호스트 CPU마다 다른 경로가 돈다).
    /// </summary>
    public static (string Target, string Architecture) TargetOf(NativeArchitecture architecture) => architecture switch
    {
        NativeArchitecture.X64 => ("sse4-i32x4", "x86-64"),
        NativeArchitecture.Arm64 => ("neon-i32x4", "aarch64"),
        _ => throw new ArgumentOutOfRangeException(nameof(architecture), architecture, "No ISPC target for this architecture.")
    };

    /// <summary>
    /// 대상 플랫폼의 고정 인자다(원본·출력·include·의존 인자는 빼고). Android는 <see cref="NotSupportedException"/>이다(쿠커는 데스크톱
    /// 호스트에서만 돈다 — BD46).
    /// </summary>
    public static IReadOnlyList<string> Arguments(NativePlatform platform, NativeArchitecture architecture)
    {
        (string target, string arch) = TargetOf(architecture);
        string os = platform switch
        {
            NativePlatform.Windows => "windows",
            NativePlatform.MacOS => "macos",
            _ => throw new NotSupportedException($"ISPC sources are not supported for {platform} targets (Docs/Design/BuildTargets section 3.12).")
        };
        List<string> arguments = ["-O2", "--target=" + target, "--arch=" + arch, "--target-os=" + os, "--opt=disable-fma", "--math-lib=default"];
        if (platform != NativePlatform.Windows)
        {
            arguments.Add("--pic");
        }

        return arguments;
    }

    /// <summary>
    /// 버전과 고정 인자를 이은 ISPC 신원이다(예: <c>ispc 1.32.0 -O2 --target=sse4-i32x4 ...</c>). 출력 byte를 정하는 값만 담는다 — 실행 파일
    /// 경로는 담지 않는다.
    /// </summary>
    public static string Identity(NativePlatform platform, NativeArchitecture architecture) =>
        "ispc " + ToolchainTable.IspcVersion + " " + string.Join(' ', Arguments(platform, architecture));

    /// <summary>
    /// ISPC 원본을 가진 모듈이 소비자에게 내는 정의다(<c>ROCKS_ISPC_IDENTITY="..."</c> — C 문자열 리터럴). 사용 예(모듈 규칙):
    /// <code>
    /// PublicDefines = [IspcTools.IdentityDefine(context.Platform, context.Architecture)]
    /// </code>
    /// </summary>
    public static string IdentityDefine(NativePlatform platform, NativeArchitecture architecture) =>
        IdentityMacro + "=\"" + Identity(platform, architecture) + "\"";

    /// <summary>
    /// stage 실행 action(NativeAction.StageWorkingDirectory)이 절대 경로 인자를 받는 철자다: action 임시 디렉터리
    /// (<c>&lt;out&gt;/.native-tmp/&lt;guid&gt;</c>) 기준 상대 경로다. guid 한 단계는 길이와 무관하므로 임의 이름으로 계산해도 같다.
    /// </summary>
    public static string StageRelative(string output, string path) =>
        System.IO.Path.GetRelativePath(System.IO.Path.Combine(output, ".native-tmp", "stage"), path);

    /// <summary>모듈의 생성 헤더 디렉터리다(<c>&lt;out&gt;/Ispc/&lt;모듈&gt;</c> — BD48).</summary>
    public static string HeaderDirectory(string output, string module) => System.IO.Path.Combine(output, "Ispc", module);

    /// <summary>원본의 생성 헤더 파일 이름이다.</summary>
    public static string HeaderName(string source) => System.IO.Path.GetFileNameWithoutExtension(source) + HeaderSuffix;

    /// <summary>
    /// 고정 버전 ISPC를 찾는다: 환경 변수 <see cref="EnvironmentVariable"/>(environment에 있으면 그것, 없으면 프로세스 환경), 그다음 기본
    /// 설치 위치(Windows <c>%LOCALAPPDATA%\Rocks\toolchains\ispc-&lt;버전&gt;</c>, macOS <c>~/Library/Application Support/Rocks/toolchains</c>,
    /// 저장소 <c>out/toolchains</c>)다. 찾지 못하거나 <c>--version</c>이 고정 버전과 다르면 설치 명령을 담은
    /// <see cref="InvalidOperationException"/>이다.
    /// </summary>
    public static async Task<IspcTool> DiscoverAsync(string root, IReadOnlyDictionary<string, string?> environment,
        CancellationToken cancellationToken = default)
    {
        string version = ToolchainTable.IspcVersion;
        string install = $"Next: run Build/InstallIspc.ps1 (or Setup) to install ISPC {version} and set {EnvironmentVariable}.";
        string? configured = environment.FirstOrDefault(pair => pair.Key.Equals(EnvironmentVariable,
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value;
        configured = string.IsNullOrEmpty(configured) ? Environment.GetEnvironmentVariable(EnvironmentVariable) : configured;
        string? path;
        if (!string.IsNullOrEmpty(configured))
        {
            path = System.IO.Path.GetFullPath(configured);
            if (!File.Exists(path))
            {
                throw new InvalidOperationException($"{EnvironmentVariable} points to a missing file: {path}. {install}");
            }
        }
        else
        {
            path = DefaultLocations(root, version).FirstOrDefault(File.Exists);
            if (path is null)
            {
                throw new InvalidOperationException($"ISPC {version} is required to build ISPC sources but was not found " +
                    $"({EnvironmentVariable} is not set and no default install exists). {install}");
            }
        }

        path = PathSpelling.OnDisk(path);
        ProcessResult result = await ProcessRunner.RunAsync(new ProcessSpec(path, ["--version"], Directory.GetCurrentDirectory(), environment),
            cancellationToken);
        string line = result.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault() ?? "";
        if (result.ExitCode != 0 || !IsVersion(line, version))
        {
            throw new InvalidOperationException($"ISPC at {path} reports '{line}' (exit code {result.ExitCode}), but the build needs ISPC {version}. {install}");
        }

        return new IspcTool(path, version, Fingerprints.FileHash(path));
    }

    /// <summary><c>--version</c> 첫 줄이 그 버전인지다(<c>(Intel(r) ISPC), 1.32.0</c> 뒤가 공백·끝 — <c>1.32.01</c>은 다른 버전이다).</summary>
    public static bool IsVersion(string line, string version) =>
        Regex.IsMatch(line, @"\(Intel\(r\) ISPC\), " + Regex.Escape(version) + @"(?:\s|$)", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static IEnumerable<string> DefaultLocations(string root, string version)
    {
        string name = OperatingSystem.IsWindows() ? "ispc.exe" : "ispc";
        string directory = "ispc-" + version;
        if (OperatingSystem.IsWindows())
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (local.Length != 0)
            {
                yield return System.IO.Path.Combine(local, "Rocks", "toolchains", directory, name);
            }
        }
        else if (OperatingSystem.IsMacOS())
        {
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (home.Length != 0)
            {
                yield return System.IO.Path.Combine(home, "Library", "Application Support", "Rocks", "toolchains", directory, name);
            }
        }

        yield return System.IO.Path.Combine(root, "out", "toolchains", directory, name);
    }

    /// <summary>
    /// <c>-MMM</c> 의존 파일을 읽는다: 한 줄에 경로 하나(escape 없음 — 공백 경로도 그대로), 빈 줄은 건너뛴다. 상대 경로는 ISPC를 실행한
    /// 작업 디렉터리 기준이고 구분자가 섞여 나온다(실측 <c>../a b\inc.isph</c>). 정규 절대 경로를 처음 나온 순서대로 중복 없이 돌려준다.
    /// NUL이 있으면 <see cref="InvalidDataException"/>이다.
    /// </summary>
    public static IReadOnlyList<string> ParseDependencies(string text, string workingDirectory)
    {
        var seen = new HashSet<string>(NativeToolchains.PathComparer);
        var result = new List<string>();
        foreach (string raw in text.Split('\n'))
        {
            string line = raw.TrimEnd('\r');
            if (line.Trim().Length == 0)
            {
                continue;
            }

            if (line.Contains('\0'))
            {
                throw new InvalidDataException("The ISPC dependency file contains a NUL character.");
            }

            string full = System.IO.Path.GetFullPath(line, workingDirectory);
            if (seen.Add(full))
            {
                result.Add(full);
            }
        }

        return result;
    }
}
