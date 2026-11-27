using System.Text.Json;

namespace Rocks.Build;

/// <summary>
/// native action 그래프 밖 빌드 단계(메타 생성 등)의 증분 기록 파일이다(Docs/Design/BuildReuse 2.2·2.5절). 판정 규칙은 native와 같다
/// (UnityBuildPch 4.1–4.3절): 선언 입력 key가 같고, 기록한 의존 파일 내용·열거·존재 확인이 지금과 같고, 출력이 기록대로 있으면 hit다.
/// <list type="bullet">
/// <item>출력은 (크기, 수정 시각, 파일 ID)가 모두 같으면 기록 해시를 믿고, 하나라도 다르면 다시 해시한다.</item>
/// <item>출력 디렉터리 아래 기록에 없는 파일이 있으면 miss다.</item>
/// <item>판정 때 redirect가 경로를 바꿔 줄 수 있다: 이번 실행에서 다시 만든 의존 단계의 출력(게시 전)은 그 stage 파일과 대조한다
/// (같은 문서 2.5절). redirect가 바꾼 경로는 입력 메모를 거치지 않고 직접 해시한다(이번 실행이 만든 파일이다).</item>
/// <item>파일 형식이 다르거나 깨졌으면 모든 판정이 miss다(검증하지 않은 기록은 믿지 않는다) — <see cref="LoadProblem"/>이 이유다.</item>
/// </list>
/// 쓰는 쪽은 한 번에 하나다(같은 출력 폴더의 빌드 호출은 OutputInvocationLock으로 하나). 사용 예:
/// <c>var store = new StepRecordStore(path); var judgement = store.Judge(id, parts, memo); … store.Put(id, record); store.Save();</c>.
/// </summary>
public sealed class StepRecordStore
{
    /// <summary>기록 파일 형식 판이다. 다르면 모두 miss다.</summary>
    public const int Format = 1;

    private readonly string path;

    private readonly Dictionary<string, StepRecord> records;

    /// <param name="path">기록 파일 경로(예: <c>&lt;out&gt;/Metadata/meta-cache.json</c>). 없으면 빈 기록이다.</param>
    public StepRecordStore(string path)
    {
        this.path = Path.GetFullPath(path);
        records = new(StringComparer.Ordinal);
        if (!File.Exists(this.path))
        {
            return;
        }

        try
        {
            StepRecordFile? file = JsonSerializer.Deserialize<StepRecordFile>(File.ReadAllText(this.path));
            if (file is null || file.Format != Format || file.Records is null)
            {
                LoadProblem = $"record file format {file?.Format} is not {Format}";
                return;
            }

            foreach ((string id, StepRecord? record) in file.Records)
            {
                if (record?.Key is null || record.KeyParts is null || record.Outputs is null || record.OutputDirectories is null ||
                    record.Dependencies is null || record.Enumerations is null || record.Probes is null)
                {
                    LoadProblem = "record file has an incomplete record: " + id;
                    records.Clear();
                    return;
                }

                records[id] = record;
            }
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException or NotSupportedException)
        {
            LoadProblem = "record file is unreadable: " + error.Message;
            records.Clear();
        }
    }

    /// <summary>기록 파일을 읽지 못했으면 그 이유다(영어 한 줄). 그때는 모든 판정이 miss다.</summary>
    public string? LoadProblem { get; }

    /// <summary>key 항목 목록의 digest다(순서와 이름이 든다).</summary>
    public static string KeyOf(IReadOnlyList<StepKeyPart> parts) =>
        Fingerprints.Strings(["rocks-step-key-v1", .. parts.SelectMany(part => new[] { part.Name, part.Digest })]);

    /// <summary>지난 기록이다(없으면 null).</summary>
    public StepRecord? Get(string id) => records.GetValueOrDefault(id);

    /// <summary>
    /// id의 지난 기록이 지금도 맞는지 판정한다.
    /// </summary>
    /// <param name="id">단계 action 이름(예: <c>meta:World</c>).</param>
    /// <param name="parts">지금 계산한 key 항목이다.</param>
    /// <param name="memo">입력 메모다(의존 파일 내용·열거·존재 확인을 읽는다).</param>
    /// <param name="redirect">경로를 대조할 다른 경로로 바꾼다(바꾸지 않으면 null을 돌려준다).</param>
    public StepJudgement Judge(string id, IReadOnlyList<StepKeyPart> parts, InputMemo memo, Func<string, string?>? redirect = null)
    {
        if (LoadProblem is not null)
        {
            return new(false, LoadProblem);
        }

        if (!records.TryGetValue(id, out StepRecord? record))
        {
            return new(false, "no record");
        }

        if (record.Key != KeyOf(parts))
        {
            for (int index = 0; index < parts.Count; ++index)
            {
                if (index >= record.KeyParts.Count || record.KeyParts[index].Name != parts[index].Name)
                {
                    return new(false, "key layout changed at " + parts[index].Name);
                }

                if (record.KeyParts[index].Digest != parts[index].Digest)
                {
                    return new(false, "key changed: " + parts[index].Name);
                }
            }

            return new(false, "key layout changed");
        }

        foreach (StepDependency dependency in record.Dependencies)
        {
            string? redirected = redirect?.Invoke(dependency.Path);
            string target = redirected ?? dependency.Path;
            if (!File.Exists(target))
            {
                return new(false, "dependency missing: " + dependency.Path);
            }

            string hash = redirected is null ? memo.Content(target) : Fingerprints.FileHash(target);
            if (hash != dependency.Hash)
            {
                return new(false, "dependency changed: " + dependency.Path);
            }
        }

        foreach (StepEnumeration enumeration in record.Enumerations)
        {
            string? redirected = redirect?.Invoke(enumeration.Directory);
            string digest = redirected is null ? memo.Enumeration(enumeration.Directory, enumeration.Pattern, enumeration.Recursive)
                : InputMemo.EnumerationDigest(redirected, enumeration.Pattern, enumeration.Recursive);
            if (digest != enumeration.Digest)
            {
                return new(false, "directory contents changed: " + enumeration.Directory);
            }
        }

        foreach (StepProbe probe in record.Probes)
        {
            string? redirected = redirect?.Invoke(probe.Path);
            bool exists = redirected is null ? memo.Exists(probe.Path) : File.Exists(redirected);
            if (exists != probe.Exists)
            {
                return new(false, (probe.Exists ? "file disappeared: " : "file appeared: ") + probe.Path);
            }
        }

        return OutputsCurrent(record);
    }

    /// <summary>id의 기록을 바꾼다(저장은 <see cref="Save"/>).</summary>
    public void Put(string id, StepRecord record) => records[id] = record;

    /// <summary>기록 파일을 원자적으로 쓴다(내용이 같으면 쓰지 않는다).</summary>
    public void Save() => AtomicFiles.Write(path, JsonSerializer.Serialize(new StepRecordFile(Format,
        records.OrderBy(pair => pair.Key, StringComparer.Ordinal).ToDictionary(pair => pair.Key, pair => (StepRecord?)pair.Value)),
        new JsonSerializerOptions { WriteIndented = true }));

    private static StepJudgement OutputsCurrent(StepRecord record)
    {
        foreach ((string output, StepOutput recorded) in record.Outputs)
        {
            if (!File.Exists(output))
            {
                return new(false, "output missing: " + output);
            }

            bool identified = FileIdentity.TryGet(output, out FileIdentity identity);
            if (identified && recorded.FileId.Length != 0 && identity == new FileIdentity(recorded.Size, recorded.ModifiedTicks, recorded.FileId))
            {
                continue;
            }

            if (Fingerprints.FileHash(output) != recorded.Hash)
            {
                return new(false, "output changed: " + output);
            }
        }

        var known = new HashSet<string>(record.Outputs.Keys, NativeToolchains.PathComparer);
        foreach (string directory in record.OutputDirectories)
        {
            if (!Directory.Exists(directory))
            {
                continue;
            }

            foreach (string file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            {
                if (!known.Contains(Path.GetFullPath(file)))
                {
                    return new(false, "surplus output: " + file);
                }
            }
        }

        return new(true, "");
    }

    private sealed record StepRecordFile(int Format, Dictionary<string, StepRecord?>? Records);
}
