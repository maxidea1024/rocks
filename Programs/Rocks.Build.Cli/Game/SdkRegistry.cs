using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>등록한 설치 SDK 하나다. Id는 <c>{GUID}</c>, Path는 SDK 절대 경로다.</summary>
internal sealed record SdkRegistration(string Id, string Path);

/// <summary>
/// 이 컴퓨터의 설치 SDK 등록부다(Docs/Design/ProjectFiles.ko.md 10.2절). 사용자 범위만 쓰고, 등록은 명시적으로만 한다
/// (<c>sdk-register</c>, <c>sdk --register on</c>). 읽기는 등록부를 고치지 않는다.
/// </summary>
internal interface ISdkRegistry
{
    /// <summary>등록 항목 전부다(ID 순).</summary>
    IReadOnlyList<SdkRegistration> List();

    /// <summary>경로를 등록하고 ID를 돌려준다. 같은 경로가 이미 있으면 그 ID다(UE RegisterEngineInstallation처럼 새 GUID를 만든다).</summary>
    string Register(string path);

    /// <summary>ID 또는 경로로 지운다. 지웠으면 true다.</summary>
    bool Unregister(string idOrPath);
}

/// <summary>
/// 등록부를 고르는 곳이다: Windows는 <c>HKCU\Software\Rocks\Sdks</c>, 그 밖은 <c>~/.config/Rocks/Sdks.json</c>. 환경 변수
/// <see cref="OverrideVariable"/>가 있으면 호스트와 무관하게 그 JSON 파일이다(시험·CI가 사용자 등록부를 건드리지 않고 등록·해석을 돌린다).
/// </summary>
internal static class SdkRegistries
{
    /// <summary>Windows 등록부 키다(HKCU 아래).</summary>
    public const string WindowsKey = @"Software\Rocks\Sdks";

    /// <summary>등록부 JSON 파일을 대신 지정하는 환경 변수다.</summary>
    public const string OverrideVariable = "ROCKS_SDK_REGISTRY";

    public static ISdkRegistry Default() => Environment.GetEnvironmentVariable(OverrideVariable) is { Length: > 0 } file
        ? new JsonSdkRegistry(System.IO.Path.GetFullPath(file))
        : OperatingSystem.IsWindows()
        ? new WindowsSdkRegistry(WindowsKey)
        : new JsonSdkRegistry(System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".config", "Rocks", "Sdks.json"));

    // 같은 SDK인가(디스크 철자, 호스트 대소문자 규칙).
    internal static bool SamePath(string left, string right) =>
        string.Equals(System.IO.Path.TrimEndingDirectorySeparator(PathSpelling.OnDisk(System.IO.Path.GetFullPath(left))),
            System.IO.Path.TrimEndingDirectorySeparator(PathSpelling.OnDisk(System.IO.Path.GetFullPath(right))),
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    internal static string NewId() => Guid.NewGuid().ToString("B").ToUpperInvariant();

    // 등록 ID는 대소문자를 가리지 않는다(GUID).
    internal static bool SameId(string left, string right) => string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
}

/// <summary>Windows 레지스트리 등록부다. 값 이름 = ID, 값 = SDK 경로(REG_SZ). 시험은 임시 하위 키를 준다.</summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
internal sealed class WindowsSdkRegistry(string subKey) : ISdkRegistry
{
    public IReadOnlyList<SdkRegistration> List()
    {
        if (!OperatingSystem.IsWindows())
        {
            return [];
        }

        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey);
        if (key is null)
        {
            return [];
        }

        return [.. key.GetValueNames().Where(name => key.GetValue(name) is string)
            .Select(name => new SdkRegistration(name, (string)key.GetValue(name)!)).OrderBy(entry => entry.Id, StringComparer.Ordinal)];
    }

    public string Register(string path)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("The Windows SDK registry is Windows only.");
        }

        string full = PathSpelling.OnDisk(System.IO.Path.GetFullPath(path));
        if (List().FirstOrDefault(entry => SdkRegistries.SamePath(entry.Path, full)) is SdkRegistration existing)
        {
            return existing.Id;
        }

        string id = SdkRegistries.NewId();
        using var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(subKey);
        key.SetValue(id, full, Microsoft.Win32.RegistryValueKind.String);
        return id;
    }

    public bool Unregister(string idOrPath)
    {
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }

        SdkRegistration? entry = List().FirstOrDefault(value => SdkRegistries.SameId(value.Id, idOrPath) || SdkRegistries.SamePath(value.Path, idOrPath));
        if (entry is null)
        {
            return false;
        }

        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey, writable: true);
        key?.DeleteValue(entry.Id, throwOnMissingValue: false);
        return true;
    }
}

/// <summary>JSON 파일 등록부다(<c>{ "fileVersion": 1, "sdks": [ { "id": …, "path": … } ] }</c>). 파일이 없거나 깨졌으면 빈 목록이다.</summary>
internal sealed class JsonSdkRegistry(string file) : ISdkRegistry
{
    private const int FileVersion = 1;

    public IReadOnlyList<SdkRegistration> List()
    {
        if (!File.Exists(file))
        {
            return [];
        }

        try
        {
            JsonNode? root = JsonNode.Parse(File.ReadAllText(file));
            if (root?["fileVersion"]?.GetValue<int>() != FileVersion || root["sdks"] is not JsonArray sdks)
            {
                return [];
            }

            return [.. sdks.OfType<JsonObject>()
                .Select(entry => (Id: entry["id"]?.GetValue<string>(), Path: entry["path"]?.GetValue<string>()))
                .Where(entry => !string.IsNullOrEmpty(entry.Id) && !string.IsNullOrEmpty(entry.Path))
                .Select(entry => new SdkRegistration(entry.Id!, entry.Path!)).OrderBy(entry => entry.Id, StringComparer.Ordinal)];
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException)
        {
            return [];
        }
    }

    public string Register(string path)
    {
        string full = PathSpelling.OnDisk(System.IO.Path.GetFullPath(path));
        List<SdkRegistration> entries = [.. List()];
        if (entries.FirstOrDefault(entry => SdkRegistries.SamePath(entry.Path, full)) is SdkRegistration existing)
        {
            return existing.Id;
        }

        string id = SdkRegistries.NewId();
        entries.Add(new(id, full));
        Save(entries);
        return id;
    }

    public bool Unregister(string idOrPath)
    {
        List<SdkRegistration> entries = [.. List()];
        int removed = entries.RemoveAll(entry => SdkRegistries.SameId(entry.Id, idOrPath) || SdkRegistries.SamePath(entry.Path, idOrPath));
        if (removed != 0)
        {
            Save(entries);
        }

        return removed != 0;
    }

    private void Save(IEnumerable<SdkRegistration> entries)
    {
        var sdks = new JsonArray([.. entries.OrderBy(entry => entry.Id, StringComparer.Ordinal)
            .Select(entry => (JsonNode)new JsonObject { ["id"] = entry.Id, ["path"] = entry.Path })]);
        var root = new JsonObject { ["fileVersion"] = FileVersion, ["sdks"] = sdks };
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(file)!);
        AtomicFile.WriteIfChanged(file, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }
}
