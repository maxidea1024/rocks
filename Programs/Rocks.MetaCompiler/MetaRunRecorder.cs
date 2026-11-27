using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

/// <summary>
/// MetaCompiler 실행 하나(리플렉션 생성 또는 <c>shader-parameters</c>)가 읽은 것과 걸린 시간을 모은다(Docs/Design/BuildReuse 2.4·2.8절).
/// <list type="bullet">
/// <item>의존 기록(<c>--dependency-record</c>): clang이 연 파일(<see cref="ClangInvocation"/>이 depfile로 받는다), clang 밖에서 내용을 읽은
/// 파일, 디렉터리 열거와 결과, 존재 확인과 결과. 경로만 적는다 — 내용 해시는 빌드가 입력 메모로 잰다(같은 문서 2.6절).</item>
/// <item>단계 보고(<c>--phase-report</c>): 단계별 시간, clang 실행 수와 CPU 시간, clang 자식 프로세스의 최대 working set.</item>
/// </list>
/// 실행마다 <see cref="Begin"/>으로 만들고 비동기 흐름(<see cref="AsyncLocal{T}"/>)으로 이어진다. 같은 프로세스 안에서 여러 실행이 함께
/// 돌아도(도구 시험) 서로 섞이지 않는다. 여러 스레드가 함께 부른다(검증 컴파일·탐침 — 스레드 안전).
/// </summary>
internal sealed class MetaRunRecorder
{
    /// <summary>의존 기록 파일 형식 판이다. 바꾸면 빌드의 메타 판정이 모두 miss다.</summary>
    public const int DependencyFormat = 1;

    /// <summary>단계 보고 파일 형식 판이다.</summary>
    public const int PhaseFormat = 1;

    private static readonly AsyncLocal<MetaRunRecorder?> CurrentRecorder = new();

    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private readonly Lock gate = new();

    private readonly HashSet<string> files = new(PathComparer);

    private readonly List<(string Directory, string Pattern, bool Recursive, IReadOnlyList<string> Results)> enumerations = [];

    private readonly Dictionary<string, bool> probes = new(PathComparer);

    private readonly List<string> excludedRoots = [];

    private readonly List<string> phaseOrder = [];

    private readonly Dictionary<string, TimeSpan> phases = new(StringComparer.Ordinal);

    private readonly Stopwatch total = Stopwatch.StartNew();

    private int clangRuns;

    private int depfiles;

    private TimeSpan clangCpu;

    private long clangPeakWorkingSet;

    private string? openPhase;

    private long openPhaseStart;

    private string? unrecordable;

    private MetaRunRecorder()
    {
    }

    /// <summary>지금 비동기 흐름의 기록기다. 기록을 청하지 않은 실행(단독 실행)에서도 있다 — 기록 파일만 쓰지 않는다.</summary>
    public static MetaRunRecorder? Current => CurrentRecorder.Value;

    /// <summary>새 실행의 기록을 시작하고 지금 비동기 흐름의 기록기로 둔다.</summary>
    public static MetaRunRecorder Begin()
    {
        var recorder = new MetaRunRecorder();
        CurrentRecorder.Value = recorder;
        return recorder;
    }

    /// <summary>
    /// 이 디렉터리 아래 파일은 의존 기록에 적지 않는다: 이 실행 자신의 출력 루트, 검증 임시 디렉터리(<c>.rocks-meta-validation-&lt;guid&gt;</c>).
    /// </summary>
    public void Exclude(string directory)
    {
        lock (gate)
        {
            excludedRoots.Add(Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory)));
        }
    }

    /// <summary>clang 밖에서 내용을 읽은 파일을 적는다.</summary>
    public void NoteFile(string path)
    {
        lock (gate)
        {
            files.Add(Path.GetFullPath(path));
        }
    }

    /// <summary>디렉터리 열거와 그 결과(<see cref="RecordedEnumeration.Files"/>의 결과)를 적는다.</summary>
    public void NoteEnumeration(string directory, string pattern, bool recursive, IReadOnlyList<string> results)
    {
        lock (gate)
        {
            enumerations.Add((Path.GetFullPath(directory), pattern, recursive, results));
        }
    }

    /// <summary>존재 확인과 결과를 적는다. 같은 경로를 다시 확인하면 마지막 결과가 남는다.</summary>
    public void NoteExists(string path, bool exists)
    {
        lock (gate)
        {
            probes[Path.GetFullPath(path)] = exists;
        }
    }

    /// <summary>
    /// clang 실행 하나를 적는다. <paramref name="opened"/>는 depfile의 prerequisite다(표준 입력은 depfile에 없다). usage는 그 clang
    /// 프로세스의 자원이다(호스트가 주지 못하면 null).
    /// </summary>
    public void NoteClangRun(IReadOnlyList<string> opened, ProcessUsage? usage)
    {
        lock (gate)
        {
            ++clangRuns;
            ++depfiles;
            foreach (string path in opened)
            {
                files.Add(path);
            }

            if (usage is not null)
            {
                clangCpu += usage.CpuTime;
                clangPeakWorkingSet = Math.Max(clangPeakWorkingSet, usage.PeakWorkingSetBytes);
            }
        }
    }

    /// <summary>
    /// depfile 없이 끝난 clang 실행 하나를 적는다(치명 오류로 멈춘 실행 — <see cref="ClangInvocation"/>). 실행 수만 늘고 depfile 수는 늘지 않으며,
    /// 이 실행의 의존 기록은 쓸 수 없다고 알린다(reason).
    /// </summary>
    public void NoteClangRunWithoutDependencies(ProcessUsage? usage, string reason)
    {
        lock (gate)
        {
            ++clangRuns;
            unrecordable ??= reason;
            if (usage is not null)
            {
                clangCpu += usage.CpuTime;
                clangPeakWorkingSet = Math.Max(clangPeakWorkingSet, usage.PeakWorkingSetBytes);
            }
        }
    }

    /// <summary>
    /// 지금 단계를 끝내고 name 단계를 시작한다(단계는 차례로 이어진다 — 같은 이름이 다시 오면 시간이 더해진다). 사용 예:
    /// <c>MetaRunRecorder.Current?.Mark("ast")</c>. 보고를 쓸 때 마지막 단계가 끝난다.
    /// </summary>
    public void Mark(string name)
    {
        lock (gate)
        {
            CloseOpenPhase();
            openPhase = name;
            openPhaseStart = Stopwatch.GetTimestamp();
        }
    }

    /// <summary>
    /// 기록할 수 없는 실행 방식을 알린다(예: 빌드가 넘기지 않는 단독 include 탐색). 의존 기록에 이유가 적히고, 빌드는 그 기록을 저장하지
    /// 않는다 — 조용히 빈 기록을 내지 않는다.
    /// </summary>
    public void NoteUnrecordable(string reason)
    {
        lock (gate)
        {
            unrecordable ??= reason;
        }
    }

    /// <summary>
    /// 의존 기록을 JSON으로 쓴다(UTF-8, LF). 제외 디렉터리 아래 파일과 표준 입력 표기(<c>-</c>·<c>&lt;stdin&gt;</c>)는 뺀다. 형식:
    /// <c>{ format, unrecordable(이유 또는 null), clangRuns, depfiles, files[], enumerations[{directory, pattern, recursive, results[]}], probes[{path, exists}] }</c>.
    /// </summary>
    public void WriteDependencyRecord(string path)
    {
        string text;
        lock (gate)
        {
            List<string> kept = [.. files.Where(file => !Excluded(file)).Order(StringComparer.Ordinal)];
            var document = new
            {
                format = DependencyFormat,
                unrecordable,
                clangRuns,
                depfiles,
                files = kept,
                enumerations = enumerations.Select(item => new
                {
                    directory = item.Directory,
                    pattern = item.Pattern,
                    recursive = item.Recursive,
                    results = item.Results.Where(result => !Excluded(result)).ToArray()
                }).ToArray(),
                probes = probes.Where(pair => !Excluded(pair.Key)).OrderBy(pair => pair.Key, StringComparer.Ordinal)
                    .Select(pair => new { path = pair.Key, exists = pair.Value }).ToArray()
            };
            text = JsonSerializer.Serialize(document, new JsonSerializerOptions { WriteIndented = true }).Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        }

        AtomicFile.WriteIfChanged(path, new UTF8Encoding(false).GetBytes(text));
    }

    /// <summary>
    /// 단계 보고를 JSON으로 쓴다. 형식: <c>{ format, totalMilliseconds, phases{이름: 밀리초}, clangRuns, clangCpuMilliseconds,
    /// clangPeakWorkingSetBytes }</c>. 실패한 실행에서도 쓴다(어디서 시간이 갔는지 남긴다).
    /// </summary>
    public void WritePhaseReport(string path)
    {
        string text;
        lock (gate)
        {
            CloseOpenPhase();
            var ordered = new Dictionary<string, long>(StringComparer.Ordinal);
            foreach (string name in phaseOrder)
            {
                ordered[name] = (long)phases[name].TotalMilliseconds;
            }

            var document = new
            {
                format = PhaseFormat,
                totalMilliseconds = (long)total.Elapsed.TotalMilliseconds,
                phases = ordered,
                clangRuns,
                clangCpuMilliseconds = (long)clangCpu.TotalMilliseconds,
                clangPeakWorkingSetBytes = clangPeakWorkingSet
            };
            text = JsonSerializer.Serialize(document, new JsonSerializerOptions { WriteIndented = true }).Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        }

        AtomicFile.WriteIfChanged(path, new UTF8Encoding(false).GetBytes(text));
    }

    // 표준 입력 표기와 제외 디렉터리 아래 경로다. 잠금 안에서 부른다.
    private bool Excluded(string path)
    {
        string name = Path.GetFileName(path);
        if (name is "-" or "<stdin>")
        {
            return true;
        }

        return excludedRoots.Any(root => DirectoryPath.IsAtOrUnder(path, root));
    }

    // 열린 단계의 시간을 더한다. 잠금 안에서 부른다.
    private void CloseOpenPhase()
    {
        if (openPhase is null)
        {
            return;
        }

        TimeSpan elapsed = Stopwatch.GetElapsedTime(openPhaseStart);
        if (phases.TryGetValue(openPhase, out TimeSpan previous))
        {
            phases[openPhase] = previous + elapsed;
        }
        else
        {
            phases[openPhase] = elapsed;
            phaseOrder.Add(openPhase);
        }

        openPhase = null;
    }
}
