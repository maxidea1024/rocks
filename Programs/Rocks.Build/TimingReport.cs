using System.Text.Json;

namespace Rocks.Build;

/// <summary>
/// <c>--timing</c>의 집계다(Docs/Design/UnityBuildPch.ko.md 12절). compile action마다 clang <c>-ftime-trace</c>가 낸 JSON
/// (<c>&lt;object&gt;.time.json</c> — action의 출력)을 읽어 <c>&lt;out&gt;/Timing/&lt;이름&gt;.timing.json</c>에 번역 단위별 시간, 헤더별 누적 파싱
/// 시간 상위 목록, 템플릿 인스턴스화 상위 목록을 쓴다. 공유 PCH 헤더 구성을 정하는 자료다. 시간 단위는 마이크로초다(trace와 같다).
/// 헤더 시간은 그 헤더가 include한 헤더까지 포함한 시간이고, clang 기본 세밀도(500 µs)보다 짧은 항목은 trace에 없다.
/// </summary>
internal static class TimingReport
{
    /// <summary>compile action의 time trace 출력 확장자다(object 경로 뒤에 붙는다).</summary>
    public const string TraceExtension = ".time.json";

    /// <summary>집계 파일을 두는 출력 아래 디렉터리 이름이다.</summary>
    public const string DirectoryName = "Timing";

    // 상위 목록에 싣는 항목 수다.
    private const int TopEntries = 100;

    // 헤더 목록에서 빼는 확장자다. 묶음 번역 단위는 구성원 소스를 #include하므로 trace에는 구성원도 Source 항목으로 나온다.
    private static readonly HashSet<string> SourceExtensions = new(StringComparer.OrdinalIgnoreCase) { ".c", ".cc", ".cpp", ".cxx", ".m", ".mm" };

    public static string PathOf(NativeBuildRequest request) =>
        Path.Combine(request.OutputDirectory, DirectoryName, (request.TimingLabel ?? "Build") + ".timing.json");

    public static void Write(NativeBuildRequest request, NativePlan plan)
    {
        var units = new List<object>();
        var headers = new Dictionary<string, (long Total, int Count)>(StringComparer.Ordinal);
        var templates = new Dictionary<string, (long Total, int Count)>(StringComparer.Ordinal);
        foreach (NativeAction action in plan.Actions.Where(action => action.Kind is "Compile" or "Pch"))
        {
            string? trace = action.Outputs.FirstOrDefault(output => output.EndsWith(TraceExtension, StringComparison.Ordinal));
            if (trace is null || !File.Exists(trace))
            {
                continue;
            }

            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(trace));
            if (!document.RootElement.TryGetProperty("traceEvents", out JsonElement events))
            {
                continue;
            }

            long total = Accumulate(events, headers, templates);
            units.Add(new { kind = action.Kind, module = action.Module, source = action.Source, members = action.SourceMembers.Count, microseconds = total });
        }

        object[] Top(Dictionary<string, (long Total, int Count)> table) => table
            .OrderByDescending(pair => pair.Value.Total).ThenBy(pair => pair.Key, StringComparer.Ordinal).Take(TopEntries)
            .Select(pair => (object)new { name = pair.Key, microseconds = pair.Value.Total, count = pair.Value.Count }).ToArray();
        AtomicFiles.Write(PathOf(request), JsonSerializer.Serialize(new
        {
            schema = 1,
            translationUnits = units,
            headers = Top(headers),
            templates = Top(templates)
        }, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>
    /// trace 하나의 이벤트(<c>traceEvents</c> 배열)를 집계에 더하고 그 번역 단위의 전체 시간(<c>Total ExecuteCompiler</c>)을 돌려준다.
    /// <list type="bullet">
    /// <item>헤더: <c>Source</c> 이벤트다. clang은 이것을 비동기 시작·끝 쌍(<c>ph</c> <c>b</c>·<c>e</c>, 시각 <c>ts</c>)으로 연달아 쓰므로
    /// 시작의 파일 이름(<c>args.detail</c>)과 두 시각의 차를 더한다. 길이(<c>dur</c>)를 직접 싣는 형태도 받는다. 소스 확장자의 항목(묶음
    /// 번역 단위의 구성원)은 뺀다.</item>
    /// <item>템플릿: <c>InstantiateClass</c>·<c>InstantiateFunction</c> 이벤트의 길이다.</item>
    /// </list>
    /// </summary>
    internal static long Accumulate(JsonElement events, Dictionary<string, (long Total, int Count)> headers,
        Dictionary<string, (long Total, int Count)> templates)
    {
        long total = 0;
        // 스레드마다 열려 있는 Source 시작(파일 이름, 시각)이다. 끝 이벤트에는 이름이 없다.
        var open = new Dictionary<long, (string Detail, long Start)>();
        foreach (JsonElement item in events.EnumerateArray())
        {
            if (!item.TryGetProperty("name", out JsonElement nameElement))
            {
                continue;
            }

            string name = nameElement.GetString() ?? "";
            string detail = item.TryGetProperty("args", out JsonElement arguments) && arguments.TryGetProperty("detail", out JsonElement detailElement)
                ? detailElement.GetString() ?? "" : "";
            if (item.TryGetProperty("dur", out JsonElement durationElement))
            {
                long duration = durationElement.GetInt64();
                if (name == "Total ExecuteCompiler")
                {
                    total = duration;
                }
                else if (name == "Source")
                {
                    AddHeader(headers, detail, duration);
                }
                else if (name is "InstantiateClass" or "InstantiateFunction" && detail.Length != 0)
                {
                    Add(templates, detail, duration);
                }

                continue;
            }

            if (name != "Source" || !item.TryGetProperty("ph", out JsonElement phase) || !item.TryGetProperty("ts", out JsonElement time))
            {
                continue;
            }

            long thread = item.TryGetProperty("tid", out JsonElement threadElement) ? threadElement.GetInt64() : 0;
            if (phase.GetString() == "b")
            {
                open[thread] = (detail, time.GetInt64());
            }
            else if (phase.GetString() == "e" && open.Remove(thread, out (string Detail, long Start) begin))
            {
                AddHeader(headers, begin.Detail, time.GetInt64() - begin.Start);
            }
        }

        return total;
    }

    private static void AddHeader(Dictionary<string, (long Total, int Count)> headers, string file, long duration)
    {
        if (file.Length != 0 && !SourceExtensions.Contains(Path.GetExtension(file)))
        {
            Add(headers, file, duration);
        }
    }

    private static void Add(Dictionary<string, (long Total, int Count)> table, string key, long duration)
    {
        table.TryGetValue(key, out var current);
        table[key] = (current.Total + duration, current.Count + 1);
    }
}
