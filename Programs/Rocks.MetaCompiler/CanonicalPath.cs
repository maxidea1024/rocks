namespace Rocks.MetaCompiler;

/// <summary>각 상위 구성 요소까지 symbolic link를 풀어 별칭 경로를 비교한다.</summary>
internal static class CanonicalPath
{
    public static string Resolve(string path) => Resolve(path, 0);

    private static string Resolve(string path, int depth)
    {
        if (depth > 40) throw new IOException("Too many symbolic links while resolving: " + path);
        string full = Path.GetFullPath(path);
        string root = Path.GetPathRoot(full) ?? throw new IOException("Path has no filesystem root: " + full);
        string current = root;
        foreach (string component in full[root.Length..].Split(
            [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(current, component);
            FileSystemInfo? entry = Directory.Exists(candidate) ? new DirectoryInfo(candidate) :
                File.Exists(candidate) ? new FileInfo(candidate) : null;
            FileSystemInfo? target = entry?.ResolveLinkTarget(returnFinalTarget: true);
            current = target is null ? candidate : Resolve(target.FullName, depth + 1);
        }
        return Path.GetFullPath(current);
    }

    public static bool Equals(string left, string right) => string.Equals(Resolve(left), Resolve(right),
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
}
