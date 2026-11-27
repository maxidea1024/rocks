using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 메타 action(Docs/Design/BuildReuse 2.2절)의 key·의존 기록·출력 기록을 만드는 부분이다. 판정과 저장은 <see cref="StepRecordStore"/>가 한다.
/// 경로 쌍(게시 경로, stage 경로)은 이번 실행에서 다시 만드는 출력의 대응이다: 판정 때는 게시 경로를 stage로 바꿔 대조하고(2.5절), 기록할 때는
/// stage 경로를 게시 경로로 바꿔 적는다(2.4절). stage가 null이면(셰이더 파라미터 — stage 없이 출력 루트에 직접 쓴다) 바꿀 것이 없다.
/// </summary>
internal static class MetadataActions
{
    /// <summary>key 인자 목록에서 빼는 옵션이다(값을 하나 받는다 — 출력과 무관한 병렬 수·계측 경로).</summary>
    private static readonly HashSet<string> UnkeyedOptions = new(StringComparer.Ordinal) { "--jobs", "--phase-report", "--dependency-record" };

    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    /// <summary>stage 루트를 자리 표시로 바꾸고 출력과 무관한 옵션(값 포함)을 뺀 인자 digest다.</summary>
    public static string ArgumentsDigest(IReadOnlyList<string> arguments, string? stage)
    {
        var kept = new List<string>();
        for (int index = 0; index < arguments.Count; ++index)
        {
            if (UnkeyedOptions.Contains(arguments[index]))
            {
                ++index;
                continue;
            }

            kept.Add(stage is null ? arguments[index] : ReplacePrefix(arguments[index], stage, "<STAGE>"));
        }

        return ToolchainIdentity.Digest(kept);
    }

    /// <summary>
    /// 파일 목록의 내용 digest다. stage 아래 파일(이번 실행이 만든 것)은 메모를 거치지 않고 직접 해시한다. 없는 파일은 "missing"이다.
    /// </summary>
    public static string ContentsDigest(MetadataSession session, IEnumerable<string> paths, string? stage)
    {
        var tokens = new List<string>();
        foreach (string path in paths)
        {
            string full = Path.GetFullPath(path);
            tokens.Add(stage is null ? full : ReplacePrefix(full, stage, "<STAGE>"));
            tokens.Add(!File.Exists(full) ? "missing" : stage is not null && DirectoryPath.IsAtOrUnder(full, stage) ? Hashing.Sha256File(full) : session.Memo.Content(full));
        }

        return ToolchainIdentity.Digest(tokens);
    }

    /// <summary>
    /// 게시 경로(쌍의 첫째) 아래면 그 stage 경로로 바꾸는 함수다. produced(이번 메타 단계가 나중에 쓰는 출력 루트 — 셰이더 파라미터) 아래면
    /// 같은 경로를 돌려준다(입력 메모를 거치지 않고 직접 해시하게 — 그 파일은 이번 단계 안에서 바뀔 수 있다). 둘 다 아니면 null이다.
    /// </summary>
    public static Func<string, string?> Redirect(IReadOnlyList<(string Published, string Staged)> pairs, IReadOnlyList<string> produced) => path =>
    {
        if (produced.Any(root => DirectoryPath.IsAtOrUnder(path, root)))
        {
            return path;
        }

        foreach ((string published, string staged) in pairs)
        {
            if (DirectoryPath.IsAtOrUnder(path, published))
            {
                return ReplacePrefix(path, published, staged);
            }
        }

        return null;
    };

    /// <summary>MetaCompiler 의존 기록을 읽어 만든 결과다. Problem이 있으면 기록하지 않는다(로그에 그 이유).</summary>
    public sealed record Dependencies(IReadOnlyList<StepDependency> Files, IReadOnlyList<StepEnumeration> Enumerations, IReadOnlyList<StepProbe> Probes,
        string? Problem, int ClangRuns, int Depfiles);

    /// <summary>
    /// MetaCompiler 의존 기록(<c>--dependency-record</c>)을 빌드 기록 항목으로 바꾼다(Docs/Design/BuildReuse 2.4절 제외·변환 규칙).
    /// <list type="bullet">
    /// <item>툴체인 소유 파일은 뺀다.</item>
    /// <item>이번 stage의 다른 모듈 출력은 게시 경로로 적고 해시는 stage 파일로 잰다.</item>
    /// <item>규칙으로 설명되지 않는 경로(다른 stage, 검증 임시 디렉터리, 시스템 임시 디렉터리)가 있으면 Problem이다.</item>
    /// <item>열거·존재 확인은 MetaCompiler가 본 결과와 입력 메모의 값이 같아야 한다(다르면 실행 중 바뀐 것 — Problem).</item>
    /// </list>
    /// </summary>
    public static Dependencies ReadDependencies(MetadataSession session, string recordPath, string? stage,
        IReadOnlyList<(string Published, string Staged)> stagePairs, IReadOnlyList<string> produced)
    {
        JsonNode? document;
        try
        {
            document = JsonNode.Parse(File.ReadAllText(recordPath));
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException)
        {
            return new([], [], [], "dependency record is unreadable: " + error.Message, 0, 0);
        }

        if (document?["format"]?.GetValue<int>() != 1)
        {
            return new([], [], [], "dependency record format is not 1", 0, 0);
        }

        int runs = document["clangRuns"]?.GetValue<int>() ?? -1;
        int depfiles = document["depfiles"]?.GetValue<int>() ?? -2;
        if (document["unrecordable"]?.GetValue<string>() is string unrecordable)
        {
            return new([], [], [], "MetaCompiler could not record its dependencies: " + unrecordable, runs, depfiles);
        }

        if (runs != depfiles)
        {
            return new([], [], [], $"clang runs ({runs}) and dependency files ({depfiles}) differ", runs, depfiles);
        }

        string stages = Path.Combine(session.OutputDirectory, ".metadata-stage");
        string temporary = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.GetTempPath()));
        string? Unexplained(string path)
        {
            if (path.Contains(".rocks-meta-validation-", StringComparison.OrdinalIgnoreCase))
            {
                return "validation staging path";
            }

            if (DirectoryPath.IsAtOrUnder(path, stages) && (stage is null || !DirectoryPath.IsAtOrUnder(path, stage)))
            {
                return "another metadata stage";
            }

            if (DirectoryPath.IsAtOrUnder(path, temporary) && !DirectoryPath.IsAtOrUnder(path, session.Root))
            {
                return "temporary directory";
            }

            return null;
        }

        string Published(string path)
        {
            foreach ((string published, string staged) in stagePairs)
            {
                if (DirectoryPath.IsAtOrUnder(path, staged))
                {
                    return ReplacePrefix(path, staged, published);
                }
            }

            return path;
        }

        var files = new List<StepDependency>();
        foreach (JsonNode? node in document["files"]?.AsArray() ?? [])
        {
            string path = Path.GetFullPath(node!.GetValue<string>());
            if (session.ToolchainOwned(path))
            {
                continue;
            }

            if (Unexplained(path) is string reason)
            {
                return new([], [], [], $"unexplained dependency path ({reason}): {path}", runs, depfiles);
            }

            bool staged = stage is not null && DirectoryPath.IsAtOrUnder(path, stage);
            if (staged && PathComparer.Equals(Published(path), path))
            {
                return new([], [], [], "dependency inside the stage that maps to no published output: " + path, runs, depfiles);
            }

            if (!File.Exists(path))
            {
                return new([], [], [], "dependency vanished before it was recorded: " + path, runs, depfiles);
            }

            // stage와 이번 메타 단계가 나중에 쓰는 출력(produced)은 입력 메모를 거치지 않는다(단계 안에서 바뀐다 — 안정 검사 대상이 아니다).
            bool direct = staged || produced.Any(root => DirectoryPath.IsAtOrUnder(path, root));
            files.Add(new(Published(path), direct ? Hashing.Sha256File(path) : session.Memo.Content(path)));
        }

        var enumerations = new List<StepEnumeration>();
        foreach (JsonNode? node in document["enumerations"]?.AsArray() ?? [])
        {
            string directory = Path.GetFullPath(node!["directory"]!.GetValue<string>());
            string pattern = node["pattern"]!.GetValue<string>();
            bool recursive = node["recursive"]!.GetValue<bool>();
            string seen = ToolchainIdentity.Digest((node["results"]?.AsArray() ?? []).Select(item => Path.GetRelativePath(directory, item!.GetValue<string>())));
            if (session.Memo.Enumeration(directory, pattern, recursive) != seen)
            {
                return new([], [], [], "directory contents changed while MetaCompiler ran: " + directory, runs, depfiles);
            }

            enumerations.Add(new(directory, pattern, recursive, seen));
        }

        var probes = new List<StepProbe>();
        foreach (JsonNode? node in document["probes"]?.AsArray() ?? [])
        {
            string path = Path.GetFullPath(node!["path"]!.GetValue<string>());
            bool exists = node["exists"]!.GetValue<bool>();
            if (stage is not null && DirectoryPath.IsAtOrUnder(path, stage))
            {
                probes.Add(new(Published(path), exists));
                continue;
            }

            if (session.Memo.Exists(path) != exists)
            {
                return new([], [], [], "file existence changed while MetaCompiler ran: " + path, runs, depfiles);
            }

            probes.Add(new(path, exists));
        }

        return new(files, enumerations, probes, null, runs, depfiles);
    }

    /// <summary>출력 파일(게시 경로)과 출력 디렉터리 아래 모든 파일을 잰다.</summary>
    public static Dictionary<string, StepOutput> MeasureOutputs(IEnumerable<string> files, IEnumerable<string> directories)
    {
        var outputs = new Dictionary<string, StepOutput>(PathComparer);
        foreach (string directory in directories.Where(Directory.Exists))
        {
            foreach (string file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
            {
                outputs[Path.GetFullPath(file)] = StepOutput.Measure(file);
            }
        }

        foreach (string file in files.Where(File.Exists))
        {
            outputs[Path.GetFullPath(file)] = StepOutput.Measure(file);
        }

        return outputs;
    }

    /// <summary>
    /// shadow 대조(Docs/Design/BuildReuse 2.7절): would-hit으로 판정한 action의 이번 생성물(staged — 게시 경로 → 파일 경로)과 지난 기록의
    /// 출력이 같은가. 다르면 첫 차이(영어 한 줄), 같으면 null이다.
    /// </summary>
    public static string? CompareOutputs(StepRecord previous, IReadOnlyDictionary<string, string> staged)
    {
        foreach ((string published, StepOutput output) in previous.Outputs.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (!staged.TryGetValue(published, out string? file))
            {
                return "no longer produced: " + published;
            }

            if (Hashing.Sha256File(file) != output.Hash)
            {
                return "different bytes: " + published;
            }
        }

        foreach (string published in staged.Keys.Order(StringComparer.Ordinal))
        {
            if (!previous.Outputs.ContainsKey(published))
            {
                return "newly produced: " + published;
            }
        }

        return null;
    }

    /// <summary>MetaCompiler 단계 보고를 읽는다(없거나 깨졌으면 null — 보고에만 쓴다).</summary>
    public static JsonNode? ReadPhaseReport(string path)
    {
        try
        {
            return File.Exists(path) ? JsonNode.Parse(File.ReadAllText(path)) : null;
        }
        catch (Exception error) when (error is IOException or JsonException or UnauthorizedAccessException)
        {
            Console.WriteLine($"  Rocks metadata: phase report {path} is unreadable ({error.Message}).");
            return null;
        }
    }

    /// <summary>"Rocks metadata: module X: N types; generated" 줄의 형식 수다(없으면 null).</summary>
    public static int? TypeCount(string stdout, string module)
    {
        string marker = $"module {module}: ";
        int start = stdout.IndexOf(marker, StringComparison.Ordinal);
        if (start < 0)
        {
            return null;
        }

        start += marker.Length;
        int end = start;
        while (end < stdout.Length && char.IsAsciiDigit(stdout[end]))
        {
            ++end;
        }

        return end > start && int.TryParse(stdout.AsSpan(start, end - start), System.Globalization.NumberStyles.None,
            System.Globalization.CultureInfo.InvariantCulture, out int count) ? count : null;
    }

    private static string ReplacePrefix(string value, string prefix, string replacement)
    {
        string trimmed = Path.TrimEndingDirectorySeparator(prefix);
        int at = value.IndexOf(trimmed, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
        if (at < 0)
        {
            return value;
        }

        int after = at + trimmed.Length;
        if (after < value.Length && value[after] != Path.DirectorySeparatorChar && value[after] != Path.AltDirectorySeparatorChar)
        {
            return value;
        }

        return value[..at] + replacement + value[after..];
    }
}
