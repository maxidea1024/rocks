using System.Globalization;
using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>묶음 크기 실측의 조합 하나다. <c>Unity</c>가 false면 그 모듈을 묶지 않고, 나머지 두 값은 쓰지 않는다.</summary>
public sealed record UnityProfileCase(string Name, bool Unity, int AnchorFiles, int BatchBytes);

/// <summary>
/// 조합 하나의 측정 결과다. 대상 모듈의 compile action만 센다.
/// <list type="bullet">
/// <item><c>Succeeded</c>가 false면 그 조합은 재지 못했다. <c>ExitCode</c>와 <c>Error</c>(첫 오류 한 줄)만 뜻이 있고 나머지는 0이다.</item>
/// <item><c>CpuMilliseconds</c>: 컴파일러 프로세스 CPU 시간의 합.</item>
/// <item><c>WallMilliseconds</c>: native 빌드 단계 전체의 벽시계 시간(의존 모듈은 cache hit이다).</item>
/// <item><c>PeakMemoryBytes</c>·<c>PeakCommitBytes</c>: 컴파일러 프로세스 하나의 최대 working set·최대 commit 가운데 가장 큰 값.</item>
/// <item><c>ProcessMeasured</c>: 호스트가 프로세스 계측을 줬는가. false면 CPU·메모리는 0이다.</item>
/// </list>
/// </summary>
public sealed record UnityProfileRow(UnityProfileCase Case, bool Succeeded, int ExitCode, string? Error, int TranslationUnits, long CpuMilliseconds,
    long WallMilliseconds, long PeakMemoryBytes, long PeakCommitBytes, bool ProcessMeasured)
{
    /// <summary>재지 못한 조합의 행이다. <paramref name="error"/>는 첫 줄만 남긴다.</summary>
    public static UnityProfileRow Failed(UnityProfileCase profile, int exitCode, string error) =>
        new(profile, false, exitCode, UnityProfile.FirstLine(error), 0, 0, 0, 0, 0, false);
}

/// <summary>
/// 묶음 크기 실측(<c>Rocks.Build.Cli unity-profile</c>, Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측")의 조합·결과 계산이다.
/// 모듈 하나를 "유니티 끔"과 <c>UnityAnchorFiles</c>·<c>UnityBatchBytes</c> 조합마다 처음부터 컴파일해, 모듈 규칙의 재정의 값을 고를
/// 근거를 낸다(UE <c>-Mode=ProfileUnitySizes</c> 대응). 빌드 실행은 명령줄 도구가 하고, 이 클래스는 조합 목록, 모듈 입력의 재정의,
/// 출력 디렉터리 확인, 빌드 결과에서 행을 뽑는 일, 표 출력만 한다.
/// </summary>
public static class UnityProfile
{
    /// <summary>기본으로 재는 <c>UnityAnchorFiles</c> 값이다(스펙 12절).</summary>
    public static readonly IReadOnlyList<int> DefaultAnchorFiles = [8, 16, 24, 32];

    /// <summary>기본으로 재는 묶음 상한(KiB)이다(스펙 12절).</summary>
    public static readonly IReadOnlyList<int> DefaultBatchKibibytes = [256, 384, 512];

    /// <summary>이 명령의 출력 디렉터리 이름이 가져야 하는 접두다. 평소 빌드 출력(<c>dotnet-…</c>)과 이름으로 갈린다.</summary>
    public const string OutputPrefix = "unity-profile-";

    /// <summary>이 명령이 만든 출력 디렉터리에 두는 표지 파일 이름이다.</summary>
    public const string MarkerFileName = ".unity-profile";

    // 빌드 엔진의 증분 기록 파일이다(IncrementalStore). 표지 없이 이 파일이 있으면 다른 빌드가 쓰는 출력이다.
    private const string IncrementalRecordFileName = "native-cache.json";

    private const int BytesPerKibibyte = 1024;

    private const double BytesPerMebibyte = 1024.0 * 1024.0;

    private const double MillisecondsPerSecond = 1000.0;

    private static readonly char[] PathSeparators = ['/', (char)92];

    // object 디렉터리 이름이다(ActionGraph의 <out>/obj/<모듈>/<변형>).
    private const string ObjectDirectoryName = "obj";

    /// <summary>
    /// 재는 조합이다: "유니티 끔" 하나, 그다음 <paramref name="anchorFiles"/> × <paramref name="batchKibibytes"/>를 적힌 순서로 낸다.
    /// 값이 1보다 작거나 목록이 비면 <see cref="ArgumentException"/>이다. 사용 예:
    /// <c>UnityProfile.Cases(UnityProfile.DefaultAnchorFiles, UnityProfile.DefaultBatchKibibytes)</c>(13개).
    /// </summary>
    public static IReadOnlyList<UnityProfileCase> Cases(IReadOnlyList<int> anchorFiles, IReadOnlyList<int> batchKibibytes)
    {
        if (anchorFiles.Count == 0 || batchKibibytes.Count == 0 || anchorFiles.Any(value => value < 1) ||
            batchKibibytes.Any(value => value < 1 || value > int.MaxValue / BytesPerKibibyte))
        {
            throw new ArgumentException("unity-profile needs at least one positive anchor file count and one positive batch size.");
        }

        var cases = new List<UnityProfileCase> { new("unity off", false, 0, 0) };
        foreach (int files in anchorFiles)
        {
            foreach (int kibibytes in batchKibibytes)
            {
                cases.Add(new($"anchor {files}, {kibibytes} KiB", true, files, kibibytes * BytesPerKibibyte));
            }
        }

        return cases;
    }

    /// <summary>
    /// 조합을 모듈 입력에 재정의로 얹는다(그 모듈만 바뀌므로 의존 모듈의 object는 cache hit이다). 끄는 조합은 손으로 쓴 소스와
    /// 리플렉션 생성 소스를 모두 단독 번역 단위로 둔다. 켜는 조합의 두 값은 두 묶음 구성에 함께 듣는다(생성 소스를 묶는지는 모듈
    /// 설정 그대로다).
    /// </summary>
    public static NativeModuleInput Apply(NativeModuleInput module, UnityProfileCase profile) => profile.Unity
        ? module with { Unity = UnityMode.On, UnityAnchorFiles = profile.AnchorFiles, UnityBatchBytes = profile.BatchBytes }
        : module with { Unity = UnityMode.Off, ReflectionUnity = false };

    /// <summary>
    /// 조합마다 처음부터 컴파일하게 하려고 지우는 디렉터리다: 그 모듈의 object(모든 변형)와 묶음 파일, 그리고 PCH 전체(한 번만 만들면
    /// 처음 재는 조합의 벽시계에만 들어가므로 조합마다 다시 만든다). 지워도 되는 경로만 돌려준다: <paramref name="module"/>이 경로 조각
    /// 하나가 아니거나(<c>..</c>, 구분자, 빈 이름) 결과가 <paramref name="output"/> 바로 아래를 벗어나면 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static IReadOnlyList<string> ModuleOutputDirectories(string output, string module)
    {
        // 구분자는 호스트와 상관없이 둘 다 거부한다(POSIX에서 역슬래시는 파일 이름 문자지만 모듈 이름에는 쓰지 않는다).
        if (string.IsNullOrWhiteSpace(module) || module is "." or ".." || module.IndexOfAny(PathSeparators) >= 0 ||
            Path.GetFileName(module) != module || module.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            throw new ArgumentException($"'{module}' is not a module name (it must be a single path segment).");
        }

        string root = Path.GetFullPath(output);
        string[] directories =
        [
            Path.GetFullPath(Path.Combine(root, ObjectDirectoryName, module)), Path.GetFullPath(UnityBuild.DirectoryOf(root, module)),
            Path.GetFullPath(Path.Combine(root, PchBuild.DirectoryName))
        ];
        foreach (string directory in directories)
        {
            if (!DirectoryPath.IsStrictlyUnder(directory, root))
            {
                throw new ArgumentException($"Refusing to delete {directory}: it is not under the output directory {root}.");
            }
        }

        return directories;
    }

    /// <summary>
    /// <paramref name="output"/>을 이 명령의 출력으로 삼는다(명령이 빌드 전에 한 번 부른다). 폴더 이름이 <see cref="OutputPrefix"/>로
    /// 시작해야 하고, 표지 파일 없이 증분 기록이 있는 디렉터리(다른 빌드의 출력)는 거부한다(<see cref="ArgumentException"/>). 통과하면
    /// 디렉터리와 표지 파일을 만든다. 이 명령은 그 모듈의 object를 지우고 compile DB·보고서·증분 기록을 덮어쓰므로 평소 빌드 출력에 대고
    /// 돌면 안 된다.
    /// </summary>
    public static void ClaimOutput(string output)
    {
        string directory = Path.GetFullPath(output);
        RequireOutputName(directory);
        string marker = Path.Combine(directory, MarkerFileName);
        if (File.Exists(marker))
        {
            return;
        }

        if (File.Exists(Path.Combine(directory, IncrementalRecordFileName)))
        {
            throw new ArgumentException($"{directory} holds the outputs of another build (it has {IncrementalRecordFileName} but no {MarkerFileName} " +
                "marker); unity-profile deletes module objects and overwrites build reports, so give it its own output directory.");
        }

        Directory.CreateDirectory(directory);
        File.WriteAllText(marker, "Output directory of Rocks.Build.Cli unity-profile. Module objects here are deleted and rebuilt for every case.\n");
    }

    /// <summary>
    /// <paramref name="output"/>이 <see cref="ClaimOutput"/>을 거친 출력인지 확인한다(이름 접두와 표지 파일). build 흐름이 조합을 얹기 전에
    /// 부른다. 아니면 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static void RequireClaimedOutput(string output)
    {
        string directory = Path.GetFullPath(output);
        RequireOutputName(directory);
        if (!File.Exists(Path.Combine(directory, MarkerFileName)))
        {
            throw new ArgumentException($"{directory} is not a unity-profile output directory (no {MarkerFileName} marker).");
        }
    }

    private static void RequireOutputName(string directory)
    {
        string name = Path.GetFileName(Path.TrimEndingDirectorySeparator(directory));
        if (!name.StartsWith(OutputPrefix, StringComparison.Ordinal) || name.Length == OutputPrefix.Length)
        {
            throw new ArgumentException($"The unity-profile output directory name must start with '{OutputPrefix}': {directory}");
        }
    }

    /// <summary>
    /// 빌드 결과에서 <paramref name="module"/>의 compile action을 모아 행을 만든다. 빌드는
    /// <see cref="NativeBuildRequest.MeasureProcesses"/>를 켜고 돌린 것이어야 한다. 그 모듈의 compile action이 하나도 없거나, 실행되지 않은
    /// 것(cache hit)이 있으면 처음부터 컴파일한 측정이 아니므로 <see cref="InvalidOperationException"/>이다.
    /// </summary>
    public static UnityProfileRow Row(UnityProfileCase profile, NativeBuildResult result, string module, TimeSpan wall)
    {
        NativeActionResult[] compiles = result.Actions
            .Where(action => action.Kind == "Compile" && string.Equals(action.Module, module, StringComparison.OrdinalIgnoreCase)).ToArray();
        if (compiles.Length == 0)
        {
            throw new InvalidOperationException($"Module {module} has no compile action in this build.");
        }

        int cached = compiles.Count(action => !action.Executed);
        if (cached != 0)
        {
            throw new InvalidOperationException(
                $"Module {module}: {cached} of {compiles.Length} compile actions were not executed, so this is not a clean compile ({profile.Name}).");
        }

        return new(profile, true, 0, null, compiles.Length, compiles.Sum(action => action.CpuMilliseconds), (long)wall.TotalMilliseconds,
            compiles.Max(action => action.PeakMemoryBytes), compiles.Max(action => action.PeakCommitBytes), compiles.All(action => action.ProcessMeasured));
    }

    /// <summary>
    /// 결과 표다(영어, 줄마다 조합 하나). 프로세스 계측이 없는 행은 CPU·메모리 칸이 <c>n/a</c>다. 재지 못한 조합은 칸 대신
    /// <c>failed (exit N)</c>을 쓰고, 표 아래에 조합마다 첫 오류 한 줄을 적는다.
    /// </summary>
    public static string Table(string module, IReadOnlyList<UnityProfileRow> rows)
    {
        const string header = "case";
        int width = Math.Max(header.Length, rows.Count == 0 ? 0 : rows.Max(row => row.Case.Name.Length));
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Unity profile: module {module}\n");
        text.Append(CultureInfo.InvariantCulture, $"{header.PadRight(width)}  {"TUs",5}  {"CPU s",9}  {"wall s",8}  {"peak MiB",9}  {"commit MiB",10}\n");
        foreach (UnityProfileRow row in rows)
        {
            if (!row.Succeeded)
            {
                text.Append(CultureInfo.InvariantCulture, $"{row.Case.Name.PadRight(width)}  failed (exit {row.ExitCode})\n");
                continue;
            }

            string cpu = row.ProcessMeasured ? (row.CpuMilliseconds / MillisecondsPerSecond).ToString("F1", CultureInfo.InvariantCulture) : "n/a";
            string peak = row.ProcessMeasured ? (row.PeakMemoryBytes / BytesPerMebibyte).ToString("F0", CultureInfo.InvariantCulture) : "n/a";
            string commit = row.ProcessMeasured ? (row.PeakCommitBytes / BytesPerMebibyte).ToString("F0", CultureInfo.InvariantCulture) : "n/a";
            string wall = (row.WallMilliseconds / MillisecondsPerSecond).ToString("F1", CultureInfo.InvariantCulture);
            text.Append(CultureInfo.InvariantCulture,
                $"{row.Case.Name.PadRight(width)}  {row.TranslationUnits,5}  {cpu,9}  {wall,8}  {peak,9}  {commit,10}\n");
        }

        foreach (UnityProfileRow row in rows.Where(row => !row.Succeeded))
        {
            text.Append(CultureInfo.InvariantCulture, $"error: {row.Case.Name}: {row.Error}\n");
        }

        return text.ToString();
    }

    /// <summary>여러 줄 오류 문구의 비지 않은 첫 줄이다(없으면 빈 문자열).</summary>
    public static string FirstLine(string? text) => Lines(text).FirstOrDefault() ?? "";

    /// <summary>
    /// 도구 출력에서 오류를 알리는 첫 줄이다("error"가 든 줄 — 컴파일러의 "In file included from" 같은 앞줄을 건너뛴다). 그런 줄이
    /// 없으면 비지 않은 첫 줄이다.
    /// </summary>
    public static string FirstErrorLine(string? text) =>
        Lines(text).FirstOrDefault(line => line.Contains("error", StringComparison.OrdinalIgnoreCase)) ?? FirstLine(text);

    private static string[] Lines(string? text) =>
        (text ?? "").Split('\n', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
}
