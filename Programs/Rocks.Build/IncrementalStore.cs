using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Rocks.Build;

internal sealed record CachedAction(string Key, Dictionary<string, string> Outputs);

internal sealed class IncrementalStore
{
    private readonly string path;
    private readonly ConcurrentDictionary<string, CachedAction> actions;

    public IncrementalStore(string output)
    {
        path = Path.Combine(output, "native-cache.json");
        Dictionary<string, CachedAction>? loaded = null;
        try { if (File.Exists(path)) loaded = JsonSerializer.Deserialize<Dictionary<string, CachedAction>>(File.ReadAllText(path)); }
        catch (JsonException) { /* 손상된 index는 miss이며, 검증하지 않은 출력은 재사용하지 않는다. */ }
        actions = new(loaded ?? new Dictionary<string, CachedAction>(), StringComparer.Ordinal);
    }

    public bool IsCurrent(NativeAction action, string key)
    {
        if (!actions.TryGetValue(action.Id, out var cached) || cached.Key != key || cached.Outputs.Count != action.Outputs.Count)
            return false;
        foreach (string output in action.Outputs)
            if (!cached.Outputs.TryGetValue(output, out string? digest) || !File.Exists(output) || Fingerprints.FileHash(output) != digest)
                return false;
        return true;
    }

    public void Publish(NativeAction action, string key) => actions[action.Id] =
        new CachedAction(key, action.Outputs.ToDictionary(value => value, Fingerprints.FileHash, StringComparer.Ordinal));

    public void Forget(string id) => actions.TryRemove(id, out _);

    public void Save() => AtomicFiles.Write(path, JsonSerializer.Serialize(actions.OrderBy(pair => pair.Key, StringComparer.Ordinal)
        .ToDictionary(pair => pair.Key, pair => pair.Value), new JsonSerializerOptions { WriteIndented = true }));
}

internal static class AtomicFiles
{
    // 같은 디렉터리에서 replace한다. 실패한 compiler의 임시 출력은 이 경로에 도달하지 않는다.
    public static void Write(string path, string content)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        if (File.Exists(path) && File.ReadAllText(path) == content) return;
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, content, new UTF8Encoding(false)); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
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

    private static readonly HashSet<string> HeaderExtensions = new(StringComparer.OrdinalIgnoreCase)
        { ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp" };

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
                    IsUnder(directory, Path.Combine(buildRoot, "out")))) continue;
            foreach (string file in Directory.EnumerateFiles(directory).Order(StringComparer.Ordinal))
                if (!headersOnly || HeaderExtensions.Contains(Path.GetExtension(file))) yield return Path.GetFullPath(file);
            foreach (string child in Directory.EnumerateDirectories(directory).OrderDescending(StringComparer.Ordinal))
                if (Path.GetFileName(child) != ".git" && (!headersOnly || Path.GetFileName(child) != ".native-tmp")) pending.Push(child);
        }
    }

    public static bool IsUnder(string path, string root)
    {
        string prefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)) + Path.DirectorySeparatorChar;
        return NativeToolchains.PathComparer.Equals(Path.GetFullPath(path), Path.TrimEndingDirectorySeparator(root)) ||
            path.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
    }

    public static string Paths(IEnumerable<string> paths, bool headersOnly = false, string? exclude = null, string? buildRoot = null)
    {
        var files = new SortedSet<string>(NativeToolchains.PathComparer);
        var tokens = new List<string>();
        foreach (string path in paths.Distinct(NativeToolchains.PathComparer))
        {
            tokens.Add(path);
            if (File.Exists(path)) files.Add(path);
            else if (Directory.Exists(path))
                foreach (string file in TreeFiles(path, headersOnly, exclude, buildRoot)) files.Add(file);
            else throw new FileNotFoundException("Declared build input does not exist.", path);
        }
        foreach (string file in files) { tokens.Add(file); tokens.Add(FileHash(file)); }
        return Strings(tokens);
    }

    public static string Toolchain(NativeToolchain toolchain)
    {
        var files = new List<string> { toolchain.CompilerPath, toolchain.ArchiverPath };
        if (toolchain.LinkerPath != null) files.Add(toolchain.LinkerPath);
        files.AddRange(toolchain.FingerprintPaths);
        files.AddRange(toolchain.RuntimeFiles);
        return Strings(new[] { "rocks-native-v1", toolchain.Identity, toolchain.Platform.ToString(), Paths(files) }
            .Concat(toolchain.Environment.OrderBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => pair.Key + "=" + pair.Value)));
    }
}
