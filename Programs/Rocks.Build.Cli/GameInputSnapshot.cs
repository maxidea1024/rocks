using System.Security.Cryptography;
using System.Text;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>규칙 평가와 메타 생성부터 링크 종료까지 게임 입력과 디렉터리 구성을 확인한다.</summary>
public static class GameInputSnapshot
{
    public static string Capture(string project, string output, string sdk, IEnumerable<string> explicitInputs)
    {
        project = Path.GetFullPath(project);
        output = Path.GetFullPath(output);
        sdk = Path.GetFullPath(sdk);
        var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        var inputs = new SortedSet<string>(comparer);
        var directories = new HashSet<string>(comparer);
        void Visit(string path)
        {
            path = Path.GetFullPath(path, project);
            if (Inside(output, path) || Inside(sdk, path))
            {
                return;
            }

            if (!File.Exists(path) && !Directory.Exists(path))
            {
                throw new FileNotFoundException("Game build input does not exist.", path);
            }

            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException("Game input snapshot cannot follow links: " + path);
            }

            if (!Directory.Exists(path))
            {
                inputs.Add(path);
                return;
            }

            if (!directories.Add(path))
            {
                return;
            }
            // 빈 디렉터리와 파일 목록의 변화도 규칙 평가 결과를 바꿀 수 있다.
            inputs.Add(path + Path.DirectorySeparatorChar);
            foreach (var child in Directory.EnumerateFileSystemEntries(path))
            {
                if (Directory.Exists(child) && Path.GetFileName(child) == ".git")
                {
                    continue;
                }

                Visit(child);
            }
        }

        Visit(project);
        foreach (var input in explicitInputs)
        {
            Visit(input);
        }

        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var input in inputs)
        {
            hash.AppendData(Encoding.UTF8.GetBytes(input + "\0"));
            if (File.Exists(input))
            {
                using var stream = File.OpenRead(input);
                hash.AppendData(SHA256.HashData(stream));
            }
        }

        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }

    public static string[] DeclaredInputs(BuildContext context, IEnumerable<NativeModuleInput> modules, TargetRules target) =>
        modules.SelectMany(module => module.Sources.Concat(module.PublicIncludeDirectories)
            .Concat(module.PrivateIncludeDirectories).Concat(module.AdditionalInputs).Concat(module.PrebuiltLibraries))
            .Concat(target.Metadata.Select(unit => unit.Input))
            .Concat(target.IdentityManifest is null ? [] : [target.IdentityManifest])
            .Select(context.PathInRoot).Distinct().ToArray();

    private static bool Inside(string root, string path)
    {
        var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        return string.Equals(root, path, comparison) || path.StartsWith(Path.TrimEndingDirectorySeparator(root) + Path.DirectorySeparatorChar, comparison);
    }
}
