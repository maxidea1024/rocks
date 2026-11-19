using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.Build;

// 출력 하나의 기록이다: 내용 해시와, 그 해시를 잰 때의 (크기, 수정 시각, 파일 ID)다(Docs/Design/UnityBuildPch.ko.md 4.3절). FileId가 빈
// 문자열이면 파일 ID를 얻지 못한 것이고 그 출력은 검사 때마다 다시 해시한다.
internal sealed record CachedOutput(string Hash, long Size, long ModifiedTicks, string FileId);

internal sealed record CachedAction(string Key, Dictionary<string, CachedOutput> Outputs);

// native-cache.json의 형식이다. Format이 다르거나 읽지 못하면 전부 miss다(검증하지 않은 출력은 재사용하지 않는다).
internal sealed record CachedIndex(int Format, Dictionary<string, CachedAction> Actions);

/// <summary>
/// action의 증분 기록이다. hit는 두 단으로 판정한다(Docs/Design/UnityBuildPch.ko.md 4.1절).
/// <list type="number">
/// <item>선언 입력 key(<see cref="NativeAction.Key(string, BuildInputs)"/>)가 기록과 같고 모든 출력이 기록대로 있다. 출력은 (크기, 수정
/// 시각, 파일 ID)가 모두 기록과 같으면 기록된 해시를 믿고, 하나라도 다르면 내용을 다시 해시해 대조한다(4.3절 — 수십 MB의 PCH·실행
/// 파일을 무변경 빌드마다 다시 읽지 않는다. 시각만으로 hit를 판정하지 않는다).</item>
/// <item>의존 기록이 있는 action은 지난 실행에서 컴파일러가 연 파일의 내용 해시가 지금과 모두 같다. 하나라도 다르거나 없거나 기록이
/// 깨졌으면 miss다.</item>
/// </list>
/// </summary>
internal sealed class IncrementalStore
{
    // 2: 출력 기록에 (크기, 수정 시각, 파일 ID)를 더했고 compile action의 출력에 의존 기록이 들었다(key 구성도 2단으로 바뀌었다).
    private const int Format = 2;

    private readonly string path;
    private readonly ConcurrentDictionary<string, CachedAction> actions;

    public IncrementalStore(string output)
    {
        path = Path.Combine(output, "native-cache.json");
        Dictionary<string, CachedAction>? loaded = null;
        try
        {
            if (File.Exists(path))
            {
                CachedIndex? index = JsonSerializer.Deserialize<CachedIndex>(File.ReadAllText(path));
                if (index is { Format: Format, Actions: not null } && index.Actions.Values.All(action => action?.Key is not null &&
                        action.Outputs is not null && action.Outputs.Values.All(output => output?.Hash is not null && output.FileId is not null)))
                {
                    loaded = index.Actions;
                }
            }
        }
        catch (JsonException) { /* 손상되었거나 이전 형식인 index는 miss이며, 검증하지 않은 출력은 재사용하지 않는다. */ }

        actions = new(loaded ?? new Dictionary<string, CachedAction>(), StringComparer.Ordinal);
    }

    /// <summary>inputs가 null이면(action 하나만 보는 시험) 메모 없이 파일을 직접 읽는다.</summary>
    public bool IsCurrent(NativeAction action, string key, BuildInputs? inputs = null)
    {
        if (!actions.TryGetValue(action.Id, out var cached) || cached.Key != key || cached.Outputs.Count != action.Outputs.Count)
        {
            return false;
        }

        var verified = new Dictionary<string, CachedOutput>(StringComparer.Ordinal);
        bool refreshed = false;
        foreach (string output in action.Outputs)
        {
            if (!cached.Outputs.TryGetValue(output, out CachedOutput? record) || !File.Exists(output))
            {
                return false;
            }

            bool identified = FileIdentity.TryGet(output, out FileIdentity identity);
            if (identified && record.FileId.Length != 0 && identity == new FileIdentity(record.Size, record.ModifiedTicks, record.FileId))
            {
                verified.Add(output, record);
                continue;
            }

            if (Fingerprints.FileHash(output) != record.Hash)
            {
                return false;
            }

            // 내용은 같고 메타데이터만 바뀌었다(복사·복원 등). 다음 검사가 다시 읽지 않게 기록을 고친다.
            verified.Add(output, identified ? new(record.Hash, identity.Size, identity.ModifiedTicks, identity.FileId) : record with { FileId = "" });
            refreshed = true;
        }

        if (action.DependencyRecord is string dependencyRecord)
        {
            IReadOnlyList<DependencyEntry>? dependencies = DependencyRecord.Read(dependencyRecord);
            if (dependencies is null)
            {
                return false;
            }

            foreach (DependencyEntry dependency in dependencies)
            {
                if (!File.Exists(dependency.Path) ||
                    (inputs is null ? Fingerprints.FileHash(dependency.Path) : inputs.Content(dependency.Path)) != dependency.Hash)
                {
                    return false;
                }
            }
        }

        if (refreshed)
        {
            actions[action.Id] = new CachedAction(key, verified);
        }

        foreach ((string output, CachedOutput record) in verified)
        {
            inputs?.RecordProduced(output, record.Hash);
        }

        return true;
    }

    public void Publish(NativeAction action, string key, BuildInputs? inputs = null)
    {
        var outputs = new Dictionary<string, CachedOutput>(StringComparer.Ordinal);
        foreach (string output in action.Outputs)
        {
            // 해시를 먼저 재고 그 뒤의 메타데이터를 적는다. 사이에 파일이 바뀌면 다음 검사에서 메타데이터가 같아도 해시는 낡은 값이지만,
            // 출력은 이 빌드의 잠금 아래에서 이 action만 쓴다.
            string hash = Fingerprints.FileHash(output);
            outputs.Add(output, FileIdentity.TryGet(output, out FileIdentity identity)
                ? new(hash, identity.Size, identity.ModifiedTicks, identity.FileId) : new(hash, 0, 0, ""));
            inputs?.RecordProduced(output, hash);
        }

        actions[action.Id] = new CachedAction(key, outputs);
    }

    public void Forget(string id) => actions.TryRemove(id, out _);

    public void Save() => AtomicFiles.Write(path, JsonSerializer.Serialize(new CachedIndex(Format, actions.OrderBy(pair => pair.Key, StringComparer.Ordinal)
        .ToDictionary(pair => pair.Key, pair => pair.Value)), new JsonSerializerOptions { WriteIndented = true }));
}

internal static class AtomicFiles
{
    // 같은 디렉터리에서 replace한다. 실패한 compiler의 임시 출력은 이 경로에 도달하지 않는다.
    public static void Write(string path, string content)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        if (File.Exists(path) && File.ReadAllText(path) == content)
        {
            return;
        }

        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, content, new UTF8Encoding(false));
            File.Move(temporary, path, true);
        }
        finally
        {
            if (File.Exists(temporary))
            {
                File.Delete(temporary);
            }
        }
    }
}

internal static class Fingerprints
{
    public static string FileHash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }

    public static string Strings(IEnumerable<string> inputs)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (string input in inputs)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(input);
            digest.AppendData(BitConverter.GetBytes(bytes.Length));
            digest.AppendData(bytes);
        }

        return Convert.ToHexStringLower(digest.GetHashAndReset());
    }

    // .c도 포함한다: 외부 C 라이브러리는 다른 .c를 include해 한 번역 단위로 빌드할 수 있다
    // (예: LZ4의 lz4hc.c가 lz4.c를 포함). include된 .c가 바뀌면 그 번역 단위를 다시 컴파일해야 한다.
    internal static readonly HashSet<string> HeaderExtensions = new(StringComparer.OrdinalIgnoreCase)
        { ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp", ".c" };

    /// <summary>
    /// root 아래 파일을 결정적 순서로 연다. headersOnly 열거(include 디렉터리 inventory — 액션 key·InputSnapshot·도구 빌드 ID가
    /// 같은 열거를 쓴다)는 header 확장자 파일만 내고, 다음 하위 디렉터리를 더 뺀다: 점으로 시작하는 디렉터리(.claude·.vs·.idea·
    /// .native-tmp), .git(파일 또는 디렉터리 — worktree의 .git은 파일이다)을 가진 하위 디렉터리(중첩 작업 사본 — 경로마다 한 번
    /// 빌드 출력에 알린다), .native-build.lock·.rocks-invocation.lock을 가진 디렉터리(깊이와 무관한 모든 Rocks 출력). 열거 루트
    /// 자신은 빼지 않는다(out 아래를 직접 선언한
    /// Generated 같은 루트 포함). headersOnly가 아니면(DeclaredInputs·Toolchain) .git만 뺀다.
    /// </summary>
    public static IEnumerable<string> TreeFiles(string root, bool headersOnly, string? exclude = null, string? buildRoot = null)
    {
        var pending = new Stack<string>();
        var visited = new HashSet<string>(NativeToolchains.PathComparer);
        pending.Push(Path.GetFullPath(root));
        while (pending.Count != 0)
        {
            string directory = pending.Pop();
            var directoryInfo = new DirectoryInfo(directory);
            string identity = directoryInfo.ResolveLinkTarget(true)?.FullName ?? directoryInfo.FullName;
            // root include가 out의 병렬 action 임시 폴더까지 탐색하면 생성/삭제와 경합한다.
            // Generated/SDK include처럼 out 내부를 직접 선언한 root는 독립 입력으로 검사한다.
            if (!visited.Add(identity) ||
                (exclude != null && !IsUnder(root, exclude) && IsUnder(directory, exclude)) ||
                (headersOnly && buildRoot != null && !IsUnder(root, Path.Combine(buildRoot, "out")) &&
                    IsUnder(directory, Path.Combine(buildRoot, "out"))))
            {
                continue;
            }

            foreach (string file in Directory.EnumerateFiles(directory).Order(StringComparer.Ordinal))
            {
                if (!headersOnly || HeaderExtensions.Contains(Path.GetExtension(file)))
                {
                    yield return Path.GetFullPath(file);
                }
            }

            foreach (string child in Directory.EnumerateDirectories(directory).OrderDescending(StringComparer.Ordinal))
            {
                if (!ExcludedChild(child, headersOnly))
                {
                    pending.Push(child);
                }
            }
        }
    }

    private static bool ExcludedChild(string child, bool headersOnly)
    {
        string name = Path.GetFileName(child);
        if (name == ".git")
        {
            return true;
        }

        if (!headersOnly)
        {
            return false;
        }
        // 실측: .claude/worktrees 아래 헤더가 FoundationTests 액션 key에 들어가 다른 작업 사본의 편집이 이 빌드를 깨뜨렸다.
        // Rocks 출력 표지는 네이티브 엔진 잠금(.native-build.lock)과 CLI 호출 잠금(.rocks-invocation.lock) 둘이다.
        if (name.StartsWith('.') || File.Exists(Path.Combine(child, ".native-build.lock")) ||
            File.Exists(Path.Combine(child, ".rocks-invocation.lock")))
        {
            return true;
        }

        string git = Path.Combine(child, ".git");
        if (!File.Exists(git) && !Directory.Exists(git))
        {
            return false;
        }

        ReportNestedWorkingCopy(child);
        return true;
    }

    private static readonly HashSet<string> ReportedWorkingCopies = new(NativeToolchains.PathComparer);

    // include inventory에서 중첩 작업 사본을 뺄 때 경로마다 프로세스에서 한 번 빌드 출력에 알린다(그 안의 헤더 편집이 이 빌드를
    // 다시 실행하지 않는 이유를 사용자가 알 수 있게).
    private static void ReportNestedWorkingCopy(string directory)
    {
        lock (ReportedWorkingCopies)
        {
            if (!ReportedWorkingCopies.Add(Path.GetFullPath(directory)))
            {
                return;
            }
        }

        Console.WriteLine($"Build inputs: skipped nested working copy {directory} (has .git) in the include directory inventory.");
    }

    // path가 root 자신이거나 그 아래인지다. 드라이브 루트(W:\)도 root가 될 수 있다(DirectoryPath).
    public static bool IsUnder(string path, string root) => DirectoryPath.IsAtOrUnder(path, root);

    // memo가 있으면 디렉터리 열거와 원본 byte digest를 그 메모에서 가져온다(BuildInfo Prepare가 읽은 값 — 첫 InputSnapshot).
    // skip이 참인 파일은 열거 결과에서 뺀다(<out>/BuildInfo — Prepare만 잠금 아래에서 쓰는 생성 파일).
    public static string Paths(IEnumerable<string> paths, bool headersOnly = false, string? exclude = null, string? buildRoot = null,
        FileDigests? memo = null, Func<string, bool>? skip = null)
    {
        var files = new SortedSet<string>(NativeToolchains.PathComparer);
        var tokens = new List<string>();
        foreach (string path in paths.Distinct(NativeToolchains.PathComparer))
        {
            tokens.Add(path);
            if (File.Exists(path))
            {
                files.Add(path);
            }
            else if (Directory.Exists(path))
            {
                foreach (string file in memo?.Tree(path, headersOnly, exclude, buildRoot) ?? TreeFiles(path, headersOnly, exclude, buildRoot))
                {
                    if (skip is null || !skip(file))
                    {
                        files.Add(file);
                    }
                }
            }
            else
            {
                throw new FileNotFoundException("Declared build input does not exist.", path);
            }
        }

        foreach (string file in files)
        {
            tokens.Add(file);
            tokens.Add(memo?.Raw(file) ?? FileHash(file));
        }

        return Strings(tokens);
    }

    public static string Toolchain(NativeToolchain toolchain)
    {
        var files = new List<string> { toolchain.CompilerPath, toolchain.ArchiverPath };
        if (toolchain.LinkerPath != null)
        {
            files.Add(toolchain.LinkerPath);
        }

        files.AddRange(toolchain.FingerprintPaths);
        files.AddRange(toolchain.RuntimeFiles);
        // 환경은 대상의 Keyed 변수만 정규 철자·ordinal 순으로 넣는다. PATH·TEMP 같은 Runtime 변수와 허용 목록 밖 변수(실행마다
        // 바뀌는 DOTNET_CLI_TELEMETRY_SESSIONID 등)는 key가 아니다 — 도구는 절대 경로와 내용 해시로 이미 key에 있다.
        return Strings(new[] { "rocks-native-v2", toolchain.Identity, toolchain.Platform.ToString(), Paths(files) }
            .Concat(KeyedEnvironment(toolchain).Select(pair => pair.Key + "=" + pair.Value)));
    }

    // 대상 플랫폼의 Keyed 변수(NativeEnvironment) 중 toolchain 환경에 있는 것을 (정규 철자, 값)의 ordinal 순으로 돌려준다.
    internal static IEnumerable<KeyValuePair<string, string?>> KeyedEnvironment(NativeToolchain toolchain)
    {
        IReadOnlyList<string> keyed = toolchain.Platform switch
        {
            NativePlatform.Windows => NativeEnvironment.WindowsKeyedVariables,
            NativePlatform.MacOS => NativeEnvironment.MacKeyedVariables,
            _ => NativeEnvironment.AndroidKeyedVariables
        };
        StringComparison comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        foreach (string name in keyed.Order(StringComparer.Ordinal))
        {
            KeyValuePair<string, string?> match = toolchain.Environment.FirstOrDefault(pair => string.Equals(pair.Key, name, comparison));
            if (match.Key is not null)
            {
                yield return new(name, match.Value);
            }
        }
    }
}
