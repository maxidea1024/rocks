using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

/// <summary>실행 hook이 없는 module template 계획이다. 적용 시 내용 hash와 경로를 다시 검사한다.</summary>
public sealed record ScaffoldPlan(int Schema, string Kind, string Target,
    Dictionary<string, string> Files, Dictionary<string, string> Hashes);

/// <summary>기존 파일을 덮어쓰지 않는 plan/apply/validate다. 출력 부모는 외부에서 병행 변경하지 않는다.</summary>
public static class ScaffoldService
{
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };
    private const int MaximumFiles = 20;
    private const int MaximumContentBytes = 1024 * 1024;

    public static ScaffoldPlan PlanModule(string repositoryRoot, string name, string outputDirectory, string planPath)
    {
        if (!Regex.IsMatch(name, "^[A-Za-z][A-Za-z0-9_]{0,63}$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)))
        {
            throw new ArgumentException("Module name must be an identifier", nameof(name));
        }

        string parent = PhysicalPath(Path.GetFullPath(outputDirectory));
        string target = Path.Combine(parent, name);
        string templates = Path.Combine(repositoryRoot, "Programs", "Scaffold", "Templates", "module");
        if (!Directory.Exists(templates))
        {
            throw new DirectoryNotFoundException(templates);
        }

        Dictionary<string, string> files = new(StringComparer.Ordinal);
        foreach (string source in Directory.EnumerateFiles(templates, "*.in").Order(StringComparer.Ordinal))
        {
            if (IsLink(source))
            {
                throw new InvalidDataException("Linked templates are not supported");
            }

            string relative = Path.GetFileName(source)[..^3].Replace("Module", name, StringComparison.Ordinal);
            files.Add(relative, File.ReadAllText(source, Utf8).Replace("@NAME@", name, StringComparison.Ordinal).Replace("\r\n", "\n", StringComparison.Ordinal));
        }

        if (files.Count == 0)
        {
            throw new InvalidDataException("No module templates found");
        }

        files.Add("module.json", JsonSerializer.Serialize(new
        {
            schema = 1,
            name,
            module_id = Guid.NewGuid().ToString(),
            integration = "explicit-build-registration-required"
        }, Json) + "\n");
        Dictionary<string, string> hashes = files.ToDictionary(pair => pair.Key, pair => Hash(pair.Value), StringComparer.Ordinal);
        ScaffoldPlan plan = new(1, "module", target, files, hashes);
        ValidatePlan(plan);
        WriteNew(planPath, JsonSerializer.SerializeToUtf8Bytes(plan, Json));
        return plan;
    }

    public static ScaffoldPlan ReadPlan(string path)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(path, Utf8));
        RejectDuplicateProperties(document.RootElement);
        ScaffoldPlan plan = document.RootElement.Deserialize<ScaffoldPlan>(Json)
            ?? throw new InvalidDataException("Empty scaffold plan");
        ValidatePlan(plan);
        return plan;
    }

    public static void Apply(string planPath)
    {
        ScaffoldPlan plan = ReadPlan(planPath);
        string target = plan.Target;
        if (Exists(target))
        {
            throw new IOException("Target already exists; files will not be overwritten");
        }

        string parent = Path.GetDirectoryName(target) ?? throw new InvalidDataException("Target has no parent");
        if (!PathsEqual(PhysicalPath(parent), parent))
        {
            throw new InvalidDataException("Linked output parents are not allowed");
        }

        Directory.CreateDirectory(parent);
        string staging = Path.Combine(parent, "." + Path.GetFileName(target) + ".stage-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        try
        {
            foreach (var pair in plan.Files.OrderBy(pair => pair.Key, StringComparer.Ordinal))
            {
                string destination = ContainedPath(staging, pair.Key);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                using FileStream file = new(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                file.Write(Utf8.GetBytes(pair.Value));
            }
            // 동일 부모 안의 rename이 게시 지점이다. 경쟁으로 target이 생겨도 overwrite하지 않는다.
            Directory.Move(staging, target);
        }
        finally
        {
            if (Directory.Exists(staging))
            {
                if (!PathsEqual(Path.GetDirectoryName(Path.GetFullPath(staging))!, parent) ||
                    !Path.GetFileName(staging).StartsWith("." + Path.GetFileName(target) + ".stage-", StringComparison.Ordinal) || IsLink(staging))
                {
                    throw new InvalidDataException("Refusing cleanup outside owned staging directory");
                }

                Directory.Delete(staging, recursive: true);
            }
        }
    }

    public static void ValidateOutput(string planPath)
    {
        ScaffoldPlan plan = ReadPlan(planPath);
        if (!Directory.Exists(plan.Target) || !PathsEqual(PhysicalPath(plan.Target), plan.Target))
        {
            throw new InvalidDataException("Generated target is missing or linked");
        }

        foreach (var pair in plan.Files)
        {
            string path = ContainedPath(plan.Target, pair.Key);
            if (!PathsEqual(PhysicalPath(path), path) || IsLink(path) ||
                !File.ReadAllBytes(path).AsSpan().SequenceEqual(Utf8.GetBytes(pair.Value)))
            {
                throw new InvalidDataException("Generated output differs from plan: " + pair.Key);
            }
        }
    }

    public static void ValidatePlan(ScaffoldPlan plan)
    {
        if (plan.Schema != 1 || plan.Kind != "module")
        {
            throw new InvalidDataException("Unsupported template schema");
        }

        if (string.IsNullOrWhiteSpace(plan.Target) || !Path.IsPathFullyQualified(plan.Target) ||
            !PathsEqual(Path.GetFullPath(plan.Target), plan.Target))
        {
            throw new InvalidDataException("Target must be an absolute normalized path");
        }

        if (plan.Files is null || plan.Hashes is null || plan.Files.Count is 0 or > MaximumFiles || plan.Files.Count != plan.Hashes.Count)
        {
            throw new InvalidDataException("Invalid file count/hash manifest");
        }

        HashSet<string> portableNames = new(StringComparer.OrdinalIgnoreCase);
        foreach (var pair in plan.Files)
        {
            ValidateRelative(pair.Key);
            if (!portableNames.Add(pair.Key))
            {
                throw new InvalidDataException("Portable path collision");
            }

            if (pair.Value is null || Utf8.GetByteCount(pair.Value) > MaximumContentBytes)
            {
                throw new InvalidDataException("Invalid template content size");
            }

            if (!plan.Hashes.TryGetValue(pair.Key, out string? hash) || hash != Hash(pair.Value))
            {
                throw new InvalidDataException("Plan content changed");
            }
        }

        foreach (string path in plan.Files.Keys)
        {
            foreach (string other in plan.Files.Keys)
            {
                if (other.StartsWith(path + "/", StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("File/directory path collision");
                }
            }
        }
    }

    private static void ValidateRelative(string relative)
    {
        if (string.IsNullOrEmpty(relative) || relative.Contains('\\') || relative.Contains(':') || relative.StartsWith('/'))
        {
            throw new InvalidDataException("Unsafe template path");
        }

        foreach (string part in relative.Split('/'))
        {
            if (part is "" or "." or ".." || part.EndsWith('.') || part.EndsWith(' ') ||
                part.Any(character => character < 32 || "<>\"|?*".Contains(character)))
            {
                throw new InvalidDataException("Unsafe template path");
            }

            string stem = part.Split('.')[0].ToUpperInvariant();
            if (stem is "CON" or "PRN" or "AUX" or "NUL" ||
                (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.Ordinal) || stem.StartsWith("LPT", StringComparison.Ordinal)) && stem[3] is >= '1' and <= '9'))
            {
                throw new InvalidDataException("Reserved template path");
            }
        }
    }

    private static string ContainedPath(string directory, string relative)
    {
        string path = Path.GetFullPath(Path.Combine(directory, relative));
        string prefix = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory)) + Path.DirectorySeparatorChar;
        if (!path.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
        {
            throw new InvalidDataException("Unsafe template path");
        }

        return path;
    }

    private static void RejectDuplicateProperties(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException("Scaffold plan must be an object");
        }

        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (JsonProperty property in value.EnumerateObject())
        {
            if (!names.Add(property.Name))
            {
                throw new InvalidDataException("Duplicate plan property");
            }

            if (property.Name is "files" or "hashes")
            {
                RejectDuplicateProperties(property.Value);
            }
        }
    }

    // 최초 plan 작성은 OS가 제공한 /var 등의 정상 별칭을 실제 경로로 고정한다.
    private static string PhysicalPath(string path)
    {
        path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        string? parent = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(path));
        if (parent is null)
        {
            return path;
        }

        string candidate = Path.Combine(PhysicalPath(parent), Path.GetFileName(path));
        if (!Exists(candidate))
        {
            return candidate;
        }

        FileSystemInfo info = Directory.Exists(candidate) ? new DirectoryInfo(candidate) : new FileInfo(candidate);
        return info.ResolveLinkTarget(returnFinalTarget: true)?.FullName ?? candidate;
    }

    private static bool Exists(string path)
    {
        try
        {
            _ = File.GetAttributes(path);
            return true;
        }
        catch (FileNotFoundException)
        {
            return false;
        }
        catch (DirectoryNotFoundException)
        {
            return false;
        }
    }

    private static bool IsLink(string path) => (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;

    private static bool PathsEqual(string left, string right) => string.Equals(left, right,
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    private static string Hash(string text) => Convert.ToHexStringLower(SHA256.HashData(Utf8.GetBytes(text)));

    private static void WriteNew(string path, byte[] bytes)
    {
        path = Path.GetFullPath(path);
        string parent = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(parent);
        string temporary = Path.Combine(parent, "." + Path.GetFileName(path) + ".tmp-" + Guid.NewGuid().ToString("N"));
        try
        {
            File.WriteAllBytes(temporary, bytes);
            File.Move(temporary, path, overwrite: false);
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
