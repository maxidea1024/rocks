using System.Collections.Concurrent;

namespace Rocks.Build;

/// <summary>
/// 빌드 호출 하나가 읽은 입력의 메모다(Docs/Design/BuildReuse 2.6절). 빌드 호출 시작 때 하나 만들어 메타 단계와 native 엔진이 함께 쓴다
/// (<see cref="NativeBuildRequest.InputMemo"/> — native <c>BuildInputs</c>가 같은 메모를 먼저 본다).
/// <list type="bullet">
/// <item><see cref="Content"/>: 파일 내용의 SHA-256이다. 처음 읽을 때의 (크기, 수정 시각, 파일 ID)를 함께 적고, 다시 물을 때 그 셋이 지금과
/// 같으면 다시 읽지 않는다. 다르면 다시 읽은 값을 돌려주지만 메모는 처음 값을 지킨다(<see cref="FirstChange"/>가 변화를 알린다).</item>
/// <item><see cref="Exists"/>·<see cref="Enumeration"/>·<see cref="Listing"/>: 존재 확인, 디렉터리 열거 결과, include 디렉터리의 헤더 이름 목록이다.
/// 처음 값을 메모한다.</item>
/// <item><see cref="Track"/>: 이름 붙인 계산(툴체인 key, 도구 디렉터리 digest)의 처음 값을 메모한다.</item>
/// </list>
/// <see cref="FirstChange"/>는 메모한 모든 것을 지금 상태와 다시 대조해 처음 달라진 항목을 알린다(안정 검사 — 다르면 그 실행의 기록을
/// 저장하지 않는다). 이번 실행이 만드는 파일(stage·출력)은 메모하지 않는다 — 호출자가 직접 해시한다. 여러 스레드가 함께 부른다.
/// 사용 예: <c>var memo = new InputMemo(); string hash = memo.Content(header); … if (memo.FirstChange() is string change) …</c>.
/// </summary>
public sealed class InputMemo
{
    private readonly ConcurrentDictionary<string, Lazy<ContentEntry>> contents = new(NativeToolchains.PathComparer);

    private readonly ConcurrentDictionary<string, bool> existence = new(NativeToolchains.PathComparer);

    private readonly ConcurrentDictionary<string, Lazy<string>> enumerations = new(StringComparer.Ordinal);

    private readonly ConcurrentDictionary<string, Lazy<string>> listings = new(StringComparer.Ordinal);

    private readonly ConcurrentDictionary<string, (Func<string> Compute, Lazy<string> Value)> tracked = new(StringComparer.Ordinal);

    /// <summary>
    /// 파일 내용의 SHA-256(소문자 hex)이다. 파일이 없으면 <see cref="FileNotFoundException"/>이다.
    /// </summary>
    public string Content(string path)
    {
        string full = Path.GetFullPath(path);
        ContentEntry entry = contents.GetOrAdd(full, key => new Lazy<ContentEntry>(() => Capture(key))).Value;
        if (entry.Identity is FileIdentity recorded && FileIdentity.TryGet(full, out FileIdentity current) && current == recorded)
        {
            return entry.Hash;
        }

        return Fingerprints.FileHash(full);
    }

    /// <summary>파일이 있는가(<see cref="File.Exists"/>). 처음 답을 메모한다.</summary>
    public bool Exists(string path) => existence.GetOrAdd(Path.GetFullPath(path), File.Exists);

    /// <summary>
    /// 디렉터리 열거 결과의 digest다(<see cref="Rocks.Tooling.Common.RecordedEnumeration.Files"/>의 결과를 디렉터리 기준 상대 경로로 —
    /// 디렉터리를 다른 곳으로 바꿔 대조할 수 있게).
    /// </summary>
    public string Enumeration(string directory, string pattern, bool recursive)
    {
        string full = Path.GetFullPath(directory);
        string key = string.Join('\0', full, pattern, recursive ? "1" : "0");
        return enumerations.GetOrAdd(key, _ => new Lazy<string>(() => EnumerationDigest(full, pattern, recursive))).Value;
    }

    /// <summary>
    /// include 디렉터리의 헤더 이름 목록 digest다(native <c>BuildInputs.Listing</c>과 같은 열거). root는 작업 사본 루트, outputRoot는 빌드 출력
    /// 폴더다(그 아래 다른 출력은 열거에서 빠진다). 디렉터리가 없으면 <see cref="DirectoryNotFoundException"/>이다.
    /// </summary>
    public string Listing(string directory, string root, string outputRoot)
    {
        string full = Path.GetFullPath(directory);
        string key = string.Join('\0', full, Path.GetFullPath(root), Path.GetFullPath(outputRoot));
        return listings.GetOrAdd(key, _ => new Lazy<string>(() => ListingDigest(full, root, outputRoot))).Value;
    }

    /// <summary>
    /// 이름 붙인 계산의 값이다. 처음 부를 때 계산해 메모하고, <see cref="FirstChange"/>가 다시 계산해 대조한다. 같은 이름은 같은 계산이어야
    /// 한다(처음 등록한 계산을 쓴다).
    /// </summary>
    public string Track(string name, Func<string> compute) => tracked.GetOrAdd(name, _ => (compute, new Lazy<string>(compute))).Value.Value;

    /// <summary>
    /// 메모한 모든 항목을 지금 상태와 대조해 처음 달라진 항목의 설명(영어 한 줄)을 돌려준다. 모두 같으면 null이다. 순서는 내용·존재·열거·이름
    /// 목록·계산이고, 같은 종류 안에서는 경로 ordinal 순이다(같은 상태에서 같은 답).
    /// </summary>
    public string? FirstChange()
    {
        foreach ((string path, Lazy<ContentEntry> lazy) in contents.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            ContentEntry entry = lazy.Value;
            if (!File.Exists(path))
            {
                return "input deleted: " + path;
            }

            if (entry.Identity is FileIdentity recorded && FileIdentity.TryGet(path, out FileIdentity current) && current == recorded)
            {
                continue;
            }

            if (Fingerprints.FileHash(path) != entry.Hash)
            {
                return "input changed: " + path;
            }
        }

        foreach ((string path, bool exists) in existence.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (File.Exists(path) != exists)
            {
                return (exists ? "file disappeared: " : "file appeared: ") + path;
            }
        }

        foreach ((string key, Lazy<string> value) in enumerations.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            string[] parts = key.Split('\0');
            if (EnumerationDigest(parts[0], parts[1], parts[2] == "1") != value.Value)
            {
                return "directory contents changed: " + parts[0];
            }
        }

        foreach ((string key, Lazy<string> value) in listings.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            string[] parts = key.Split('\0');
            if (!Directory.Exists(parts[0]) || ListingDigest(parts[0], parts[1], parts[2]) != value.Value)
            {
                return "header names changed: " + parts[0];
            }
        }

        foreach ((string name, (Func<string> Compute, Lazy<string> Value) item) in tracked.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (item.Compute() != item.Value.Value)
            {
                return "changed during the build: " + name;
            }
        }

        return null;
    }

    /// <summary>메모에 처음 값이 있고 파일의 (크기, 시각, 파일 ID)가 그때와 같으면 그 해시를 돌려준다(native BuildInputs가 쓴다).</summary>
    internal bool TryCurrent(string fullPath, out string hash)
    {
        if (contents.TryGetValue(fullPath, out Lazy<ContentEntry>? lazy) && lazy.IsValueCreated && lazy.Value.Identity is FileIdentity recorded &&
            FileIdentity.TryGet(fullPath, out FileIdentity current) && current == recorded)
        {
            hash = lazy.Value.Hash;
            return true;
        }

        hash = "";
        return false;
    }

    /// <summary>열거 결과의 digest다(디렉터리 기준 상대 경로의 ordinal 목록).</summary>
    internal static string EnumerationDigest(string directory, string pattern, bool recursive) =>
        Fingerprints.Strings(Rocks.Tooling.Common.RecordedEnumeration.Files(directory, pattern, recursive).Select(file => Path.GetRelativePath(directory, file)));

    private static string ListingDigest(string directory, string root, string outputRoot)
    {
        if (!Directory.Exists(directory))
        {
            throw new DirectoryNotFoundException(directory);
        }

        string output = Path.GetFullPath(outputRoot);
        return Fingerprints.Strings(Fingerprints.TreeFiles(directory, true, output, Path.GetFullPath(root)).Select(file => Path.GetRelativePath(directory, file)));
    }

    // 읽기 전후의 (크기, 시각, 파일 ID)가 같을 때만 그 값을 적는다. 읽는 중에 바뀌었으면 ID 없이 적어 늘 다시 읽게 한다.
    private static ContentEntry Capture(string path)
    {
        bool before = FileIdentity.TryGet(path, out FileIdentity first);
        string hash = Fingerprints.FileHash(path);
        bool after = FileIdentity.TryGet(path, out FileIdentity second);
        return new ContentEntry(hash, before && after && first == second ? first : null);
    }

    private sealed record ContentEntry(string Hash, FileIdentity? Identity);
}
