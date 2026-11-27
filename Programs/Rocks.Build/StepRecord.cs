namespace Rocks.Build;

/// <summary>
/// 빌드 단계 action(메타 생성·셰이더 파라미터 생성 등 native action 그래프 밖의 일) 하나의 지난 실행 기록이다(Docs/Design/BuildReuse
/// 2.2–2.5절). <see cref="StepRecordStore"/>가 저장하고 판정한다.
/// </summary>
/// <param name="Key">선언 입력 key다(<see cref="KeyParts"/>의 digest).</param>
/// <param name="KeyParts">key를 이루는 이름 붙인 digest다(순서 그대로). miss 원인을 항목 이름으로 알리는 데 쓴다.</param>
/// <param name="Outputs">출력 파일(정규 절대 경로) → 기록이다.</param>
/// <param name="OutputDirectories">출력만 담는 디렉터리다. 그 아래 기록에 없는 파일이 있으면 miss다.</param>
/// <param name="Dependencies">실행이 내용을 읽은 파일과 해시다.</param>
/// <param name="Enumerations">실행이 열거한 디렉터리와 결과 digest다.</param>
/// <param name="Probes">실행이 존재를 확인한 경로와 결과다.</param>
/// <param name="Count">보고용 수(예: 형식 수)다. 판정에 쓰지 않는다.</param>
public sealed record StepRecord(string Key, IReadOnlyList<StepKeyPart> KeyParts, IReadOnlyDictionary<string, StepOutput> Outputs,
    IReadOnlyList<string> OutputDirectories, IReadOnlyList<StepDependency> Dependencies, IReadOnlyList<StepEnumeration> Enumerations,
    IReadOnlyList<StepProbe> Probes, int? Count = null);

/// <summary>key 항목 하나다(이름과 digest).</summary>
public sealed record StepKeyPart(string Name, string Digest);

/// <summary>출력 하나의 기록이다: 내용 해시와 그 해시를 잴 때의 (크기, 수정 시각, 파일 ID)다. FileId가 비면 늘 다시 해시한다.</summary>
public sealed record StepOutput(string Hash, long Size, long ModifiedTicks, string FileId)
{
    /// <summary>지금 파일을 잰다(해시를 먼저 재고 그 뒤의 메타데이터를 적는다 — IncrementalStore.Publish와 같은 순서).</summary>
    public static StepOutput Measure(string path)
    {
        string hash = Fingerprints.FileHash(path);
        return FileIdentity.TryGet(path, out FileIdentity identity) ? new(hash, identity.Size, identity.ModifiedTicks, identity.FileId) : new(hash, 0, 0, "");
    }
}

/// <summary>내용을 읽은 파일 하나(정규 절대 경로)와 그 SHA-256이다.</summary>
public sealed record StepDependency(string Path, string Hash);

/// <summary>열거 하나다: 디렉터리, 패턴, 하위 포함 여부, 결과 digest(<see cref="InputMemo.Enumeration"/>와 같은 규칙).</summary>
public sealed record StepEnumeration(string Directory, string Pattern, bool Recursive, string Digest);

/// <summary>존재 확인 하나다.</summary>
public sealed record StepProbe(string Path, bool Exists);

/// <summary>판정 결과다. Hit가 거짓이면 Reason이 첫 원인(영어 한 줄)이다.</summary>
public sealed record StepJudgement(bool Hit, string Reason);
