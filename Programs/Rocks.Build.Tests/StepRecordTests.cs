using Rocks.Build;

/// <summary>
/// 빌드 단계 action 기록(Docs/Design/BuildReuse 2.5·2.6절 — 메타 단계 증분의 판정)을 검사한다. 컴파일하지 않는다(임시 파일만 쓴다).
/// <list type="bullet">
/// <item>입력 메모: 같은 파일을 다시 읽지 않고, 바뀐 파일은 다시 읽은 값을 돌려주며, 안정 검사가 내용·존재·열거·이름 목록·계산 값의 변화를 알린다.</item>
/// <item>판정: 기록 없음·key 항목 변화(항목 이름)·의존 내용·의존 삭제·열거·존재 확인·출력 삭제·출력 변경·잉여 출력·redirect(stage 대조)·깨진
/// 기록 파일·형식 판.</item>
/// <item>저장 왕복: 저장한 기록을 다시 읽어 같은 판정을 낸다.</item>
/// </list>
/// </summary>
internal static class StepRecordTests
{
    public static void Run(string root, Action<string> pass)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "step-records")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
            return path;
        }

        // 입력 메모.
        string header = Write("src/a.h", "int a;\n");
        var memo = new InputMemo();
        string first = memo.Content(header);
        Check(memo.FirstChange() is null, "an untouched memo must be stable");
        Write("src/a.h", "int a; int b;\n");
        Check(memo.Content(header) != first, "a changed file must be read again");
        Check(memo.FirstChange() is string change && change.Contains("input changed", StringComparison.Ordinal),
            "the stability check must report a changed input");
        Write("src/a.h", "int a;\n");
        Check(memo.FirstChange() is null, "restoring the bytes must make the memo stable again");
        string probe = Path.Combine(fixture, "src", "missing.h");
        Check(!memo.Exists(probe), "a missing file must be reported missing");
        Write("src/missing.h", "\n");
        Check(memo.FirstChange() is string appeared && appeared.Contains("appeared", StringComparison.Ordinal), "an appearing file must break stability");
        File.Delete(probe);
        string listed = memo.Enumeration(Path.Combine(fixture, "src"), "*.h", false);
        Write("src/b.h", "\n");
        Check(memo.FirstChange() is string enumerated && enumerated.Contains("directory contents", StringComparison.Ordinal),
            "a new file in an enumerated directory must break stability");
        File.Delete(Path.Combine(fixture, "src", "b.h"));
        Check(memo.Enumeration(Path.Combine(fixture, "src"), "*.h", false) == listed, "enumeration digests must be memoised");
        int computed = 0;
        string value = "1";
        Check(memo.Track("value", () =>
        {
            ++computed;
            return value;
        }) == "1" && memo.Track("value", () => "other") == "1" && computed == 1,
            "a tracked value must be computed once and keep its first computation");
        value = "2";
        Check(memo.FirstChange() is string tracked && tracked.Contains("value", StringComparison.Ordinal), "a tracked value change must break stability");
        value = "1";
        Check(memo.FirstChange() is null, "the memo must be stable after every change was undone");
        pass("input memo remembers first values and reports every kind of change");

        // 판정.
        string dependency = Write("src/dep.h", "struct Dep {};\n");
        string enumerated2 = Directory.CreateDirectory(Path.Combine(fixture, "public")).FullName;
        Write("public/x.h", "\n");
        string outputDirectory = Directory.CreateDirectory(Path.Combine(fixture, "generated")).FullName;
        string output = Write("generated/x.generated.h", "// generated\n");
        string descriptor = Write("meta/x.rocksmeta.json", "{}\n");
        string absent = Path.Combine(fixture, "src", "shadow.h");
        string stage = Directory.CreateDirectory(Path.Combine(fixture, "stage")).FullName;
        StepKeyPart[] parts = [new("tool", "t1"), new("inputs", "i1")];
        StepRecord Record(InputMemo with) => new(StepRecordStore.KeyOf(parts), parts,
            new Dictionary<string, StepOutput> { [output] = StepOutput.Measure(output), [descriptor] = StepOutput.Measure(descriptor) },
            [outputDirectory], [new(dependency, with.Content(dependency))],
            [new(enumerated2, "*.h", false, with.Enumeration(enumerated2, "*.h", false))], [new(absent, false)], 7);
        string recordFile = Path.Combine(fixture, "meta", "meta-cache.json");
        var store = new StepRecordStore(recordFile);
        Check(store.LoadProblem is null && !store.Judge("meta:X", parts, new InputMemo()).Hit, "a missing record must miss");
        store.Put("meta:X", Record(new InputMemo()));
        store.Save();
        var reloaded = new StepRecordStore(recordFile);
        StepJudgement Judge(StepKeyPart[]? with = null, Func<string, string?>? redirect = null) =>
            reloaded.Judge("meta:X", with ?? parts, new InputMemo(), redirect);
        Check(Judge().Hit && reloaded.Get("meta:X")?.Count == 7, "an unchanged step must hit after a save/load round trip: " + Judge().Reason);
        Check(Judge([new("tool", "t2"), new("inputs", "i1")]).Reason == "key changed: tool", "a key change must name the part: " + Judge([new("tool", "t2"), new("inputs", "i1")]).Reason);
        Check(Judge([new("tool", "t1")]) is { Hit: false }, "a key with fewer parts must miss");
        Write("src/dep.h", "struct Dep { int v; };\n");
        Check(Judge().Reason.StartsWith("dependency changed", StringComparison.Ordinal), "a changed dependency must miss: " + Judge().Reason);
        Write("src/dep.h", "struct Dep {};\n");
        File.Delete(dependency);
        Check(Judge().Reason.StartsWith("dependency missing", StringComparison.Ordinal), "a deleted dependency must miss: " + Judge().Reason);
        Write("src/dep.h", "struct Dep {};\n");
        Write("public/y.h", "\n");
        Check(Judge().Reason.StartsWith("directory contents changed", StringComparison.Ordinal), "a new enumerated file must miss: " + Judge().Reason);
        File.Delete(Path.Combine(enumerated2, "y.h"));
        Write("src/shadow.h", "\n");
        Check(Judge().Reason.StartsWith("file appeared", StringComparison.Ordinal), "an appearing probed file must miss: " + Judge().Reason);
        File.Delete(absent);
        Check(Judge().Hit, "undoing every input change must hit again: " + Judge().Reason);
        Write("generated/x.generated.h", "// edited by hand\n");
        Check(Judge().Reason.StartsWith("output changed", StringComparison.Ordinal), "an edited output must miss: " + Judge().Reason);
        Write("generated/x.generated.h", "// generated\n");
        Check(Judge().Hit, "restored output bytes must hit (hash recheck after a metadata change): " + Judge().Reason);
        File.Delete(descriptor);
        Check(Judge().Reason.StartsWith("output missing", StringComparison.Ordinal), "a deleted output must miss: " + Judge().Reason);
        Write("meta/x.rocksmeta.json", "{}\n");
        Write("generated/stale.generated.h", "\n");
        Check(Judge().Reason.StartsWith("surplus output", StringComparison.Ordinal), "a surplus output must miss: " + Judge().Reason);
        File.Delete(Path.Combine(outputDirectory, "stale.generated.h"));
        Check(Judge().Hit, "removing the surplus output must hit: " + Judge().Reason);

        // redirect: 이번 실행에서 다시 만든 의존 단계의 출력은 stage와 대조한다(같은 문서 2.5절).
        string staged = Write("stage/dep.h", "struct Dep { int changedInStage; };\n");
        Func<string, string?> toStage = path => string.Equals(path, dependency, StringComparison.OrdinalIgnoreCase) ? staged : null;
        Check(Judge(redirect: toStage).Reason.StartsWith("dependency changed", StringComparison.Ordinal),
            "a dependency regenerated in this run must be compared with its stage file: " + Judge(redirect: toStage).Reason);
        Write("stage/dep.h", "struct Dep {};\n");
        Check(Judge(redirect: toStage).Hit, "a stage file with the recorded bytes must hit");

        // 깨진 기록·형식 판.
        File.WriteAllText(recordFile, "{ not json");
        var broken = new StepRecordStore(recordFile);
        Check(broken.LoadProblem is not null && !broken.Judge("meta:X", parts, new InputMemo()).Hit, "a corrupt record file must miss everything");
        File.WriteAllText(recordFile, "{\"Format\": 999, \"Records\": {}}");
        Check(new StepRecordStore(recordFile).LoadProblem is string format && format.Contains("format", StringComparison.Ordinal),
            "another record format must miss everything");
        pass("step records judge keys, dependencies, enumerations, probes, outputs and stage redirects");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
