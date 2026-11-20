using System.Globalization;
using System.Text;

namespace Rocks.Build;

/// <summary>묶음 크기 실측의 조합 하나다. <c>Unity</c>가 false면 그 모듈을 묶지 않고, 나머지 두 값은 쓰지 않는다.</summary>
public sealed record UnityProfileCase(string Name, bool Unity, int AnchorFiles, int BatchBytes);

/// <summary>
/// 조합 하나의 측정 결과다. 대상 모듈의 compile action만 센다. <c>CpuMilliseconds</c>는 컴파일러 프로세스 CPU 시간의 합,
/// <c>WallMilliseconds</c>는 native 빌드 단계 전체의 벽시계 시간(의존 모듈은 cache hit이다), <c>PeakMemoryBytes</c>는 컴파일러 프로세스
/// 하나의 최대 working set 가운데 가장 큰 값이다. 호스트가 프로세스 계측을 주지 않으면 <c>ProcessMeasured</c>가 false이고 CPU·메모리는
/// 0이다.
/// </summary>
public sealed record UnityProfileRow(UnityProfileCase Case, int TranslationUnits, long CpuMilliseconds, long WallMilliseconds, long PeakMemoryBytes,
    bool ProcessMeasured);

/// <summary>
/// 묶음 크기 실측(<c>Rocks.Build.Cli unity-profile</c>, Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측")의 조합·결과 계산이다.
/// 모듈 하나를 "유니티 끔"과 <c>UnityAnchorFiles</c>·<c>UnityBatchBytes</c> 조합마다 처음부터 컴파일해, 모듈 규칙의 재정의 값을 고를
/// 근거를 낸다(UE <c>-Mode=ProfileUnitySizes</c> 대응). 빌드 실행은 명령줄 도구가 하고, 이 클래스는 조합 목록, 모듈 입력의 재정의,
/// 빌드 결과에서 행을 뽑는 일, 표 출력만 한다.
/// </summary>
public static class UnityProfile
{
    /// <summary>기본으로 재는 <c>UnityAnchorFiles</c> 값이다(스펙 12절).</summary>
    public static readonly IReadOnlyList<int> DefaultAnchorFiles = [8, 16, 24, 32];

    /// <summary>기본으로 재는 묶음 상한(KiB)이다(스펙 12절).</summary>
    public static readonly IReadOnlyList<int> DefaultBatchKibibytes = [256, 384, 512];

    private const int BytesPerKibibyte = 1024;

    private const double BytesPerMebibyte = 1024.0 * 1024.0;

    private const double MillisecondsPerSecond = 1000.0;

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

    /// <summary>조합을 모듈 입력에 재정의로 얹는다(그 모듈만 바뀌므로 의존 모듈의 object는 cache hit이다).</summary>
    public static NativeModuleInput Apply(NativeModuleInput module, UnityProfileCase profile) => profile.Unity
        ? module with { Unity = UnityMode.On, UnityAnchorFiles = profile.AnchorFiles, UnityBatchBytes = profile.BatchBytes }
        : module with { Unity = UnityMode.Off };

    /// <summary>
    /// 조합마다 처음부터 컴파일하게 하려고 지우는 디렉터리다: 그 모듈의 object(모든 변형)와 묶음 파일. PCH는 조합과 무관하므로 두고,
    /// 측정에도 넣지 않는다.
    /// </summary>
    public static IReadOnlyList<string> ModuleOutputDirectories(string output, string module) =>
        [Path.Combine(output, ObjectDirectoryName, module), UnityBuild.DirectoryOf(output, module)];

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

        return new(profile, compiles.Length, compiles.Sum(action => action.CpuMilliseconds), (long)wall.TotalMilliseconds,
            compiles.Max(action => action.PeakMemoryBytes), compiles.All(action => action.ProcessMeasured));
    }

    /// <summary>결과 표다(영어, 줄마다 조합 하나). 프로세스 계측이 없는 행은 CPU·메모리 칸이 <c>n/a</c>다.</summary>
    public static string Table(string module, IReadOnlyList<UnityProfileRow> rows)
    {
        const string header = "case";
        int width = Math.Max(header.Length, rows.Count == 0 ? 0 : rows.Max(row => row.Case.Name.Length));
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Unity profile: module {module}\n");
        text.Append(CultureInfo.InvariantCulture, $"{header.PadRight(width)}  {"TUs",5}  {"CPU s",9}  {"wall s",8}  {"peak MiB",9}\n");
        foreach (UnityProfileRow row in rows)
        {
            string cpu = row.ProcessMeasured ? (row.CpuMilliseconds / MillisecondsPerSecond).ToString("F1", CultureInfo.InvariantCulture) : "n/a";
            string peak = row.ProcessMeasured ? (row.PeakMemoryBytes / BytesPerMebibyte).ToString("F0", CultureInfo.InvariantCulture) : "n/a";
            string wall = (row.WallMilliseconds / MillisecondsPerSecond).ToString("F1", CultureInfo.InvariantCulture);
            text.Append(CultureInfo.InvariantCulture, $"{row.Case.Name.PadRight(width)}  {row.TranslationUnits,5}  {cpu,9}  {wall,8}  {peak,9}\n");
        }

        return text.ToString();
    }
}
