using System.Text.Json;

namespace Rocks.Build;

/// <summary>
/// <c>--timing</c>의 집계다(Docs/Design/UnityBuildPch.ko.md 12절). compile action마다 clang <c>-ftime-trace</c>가 낸 JSON
/// (<c>&lt;object&gt;.time.json</c> — action의 출력)을 읽어 <c>&lt;out&gt;/Timing/&lt;이름&gt;.timing.json</c>에 번역 단위별 시간, 헤더별 누적 파싱
/// 시간 상위 목록, 템플릿 인스턴스화 상위 목록을 쓴다. 공유 PCH 헤더 구성을 정하는 자료다. 시간 단위는 마이크로초다(trace와 같다).
/// </summary>
internal static class TimingReport
{
    /// <summary>compile action의 time trace 출력 확장자다(object 경로 뒤에 붙는다).</summary>
    public const string TraceExtension = ".time.json";

    /// <summary>집계 파일을 두는 출력 아래 디렉터리 이름이다.</summary>
    public const string DirectoryName = "Timing";

    // 상위 목록에 싣는 항목 수다.
    private const int TopEntries = 100;

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

            long total = 0;
            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(trace));
            if (!document.RootElement.TryGetProperty("traceEvents", out JsonElement events))
            {
                continue;
            }

            foreach (JsonElement item in events.EnumerateArray())
            {
                if (!item.TryGetProperty("name", out JsonElement nameElement) || !item.TryGetProperty("dur", out JsonElement durationElement))
                {
                    continue;
                }

                string name = nameElement.GetString() ?? "";
                long duration = durationElement.GetInt64();
                string detail = item.TryGetProperty("args", out JsonElement arguments) && arguments.TryGetProperty("detail", out JsonElement detailElement)
                    ? detailElement.GetString() ?? "" : "";
                if (name == "Total ExecuteCompiler")
                {
                    total = duration;
                }
                else if (name == "Source" && detail.Length != 0)
                {
                    Add(headers, detail, duration);
                }
                else if (name is "InstantiateClass" or "InstantiateFunction" && detail.Length != 0)
                {
                    Add(templates, detail, duration);
                }
            }

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

    private static void Add(Dictionary<string, (long Total, int Count)> table, string key, long duration)
    {
        table.TryGetValue(key, out var current);
        table[key] = (current.Total + duration, current.Count + 1);
    }
}
