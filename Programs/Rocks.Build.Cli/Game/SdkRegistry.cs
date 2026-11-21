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
    /// <summary>
    /// 쓸 수 있는 등록 항목 전부다(ID 순). 읽을 수 없는 항목(형식이 틀림, 상대 경로)은 빼고 그 이유를 problems에 더한다(null이면 버린다).
    /// </summary>
    IReadOnlyList<SdkRegistration> List(ICollection<string>? problems = null);

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

    // 등록·해제의 읽기-고치기-쓰기를 직렬화하는 이름 있는 mutex다(같은 사용자 세션의 두 CLI가 서로의 항목을 지우지 않게).
    private const string LockName = @"Local\Rocks.SdkRegistry";

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

    // 등록 항목의 경로는 절대 경로여야 한다(상대 경로는 읽는 프로세스의 현재 디렉터리에 따라 다른 SDK가 된다).
    internal static string? PathProblem(string id, string path) =>
        System.IO.Path.IsPathFullyQualified(path) ? null : $"registry entry {id}: '{path}' is not an absolute path; ignored";

    /// <summary>등록부 잠금 안에서 읽기-고치기-쓰기를 한다.</summary>
    internal static T Exclusive<T>(Func<T> change)
    {
        using var mutex = new Mutex(false, LockName);
        try
        {
            mutex.WaitOne();
        }
        catch (AbandonedMutexException)
        {
            // 앞 소유자가 잠금을 쥔 채 끝났다. 쓰기는 원자적 파일 교체·레지스트리 값 하나라 반쯤 쓴 상태가 없으므로 이어서 쓴다.
        }

        try
        {
            return change();
        }
        finally
        {
            mutex.ReleaseMutex();
        }
    }
}

/// <summary>Windows 레지스트리 등록부다. 값 이름 = ID, 값 = SDK 경로(REG_SZ). 시험은 임시 하위 키를 준다.</summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
internal sealed class WindowsSdkRegistry(string subKey) : ISdkRegistry
{
    public IReadOnlyList<SdkRegistration> List(ICollection<string>? problems = null)
    {
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey);
        if (key is null)
        {
            return [];
        }

        var entries = new List<SdkRegistration>();
        foreach (string name in key.GetValueNames())
        {
            if (key.GetValue(name) is not string path)
            {
                problems?.Add($"registry value {name} under HKCU\\{subKey} is not a string; ignored");
            }
            else if (SdkRegistries.PathProblem(name, path) is string problem)
            {
                problems?.Add(problem);
            }
            else
            {
                entries.Add(new(name, path));
            }
        }

        return [.. entries.OrderBy(entry => entry.Id, StringComparer.Ordinal)];
    }

    public string Register(string path)
    {
        string full = PathSpelling.OnDisk(System.IO.Path.GetFullPath(path));
        return SdkRegistries.Exclusive(() =>
        {
            if (List().FirstOrDefault(entry => SdkRegistries.SamePath(entry.Path, full)) is SdkRegistration existing)
            {
                return existing.Id;
            }

            string id = SdkRegistries.NewId();
            using var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(subKey);
            key.SetValue(id, full, Microsoft.Win32.RegistryValueKind.String);
            return id;
        });
    }

    public bool Unregister(string idOrPath) => SdkRegistries.Exclusive(() =>
    {
        // 형식이 틀린 값도 ID로는 지울 수 있다(List가 뺀 값).
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey, writable: true);
        string? name = key?.GetValueNames().FirstOrDefault(value => SdkRegistries.SameId(value, idOrPath))
            ?? List().FirstOrDefault(entry => SdkRegistries.SamePath(entry.Path, idOrPath))?.Id;
        if (key is null || name is null)
        {
            return false;
        }

        key.DeleteValue(name, throwOnMissingValue: false);
        return true;
    });
}

/// <summary>
/// JSON 파일 등록부다(<c>{ "fileVersion": 1, "sdks": [ { "id": …, "path": … } ] }</c>). 읽기는 너그럽다: 파일이 없으면 빈 목록, 깨졌거나 판을
/// 모르면 빈 목록과 이유, 틀린 항목은 빼고 이유. 쓰기는 엄격하다: 파일이 깨졌거나 판을 모르면 쓰지 않고 오류이며, 읽을 수 없는 항목과 모르는
/// 칸은 그대로 두고 다시 쓴다(다른 도구·새 판이 둔 항목을 지우지 않는다).
/// </summary>
internal sealed class JsonSdkRegistry(string file) : ISdkRegistry
{
    private const int FileVersion = 1;

    public IReadOnlyList<SdkRegistration> List(ICollection<string>? problems = null)
    {
        if (!File.Exists(file))
        {
            return [];
        }

        (JsonObject? _, JsonArray? sdks, string? problem) = Load();
        if (sdks is null)
        {
            problems?.Add(problem!);
            return [];
        }

        return Entries(sdks, problems);
    }

    public string Register(string path)
    {
        string full = PathSpelling.OnDisk(System.IO.Path.GetFullPath(path));
        return SdkRegistries.Exclusive(() =>
        {
            (JsonObject root, JsonArray sdks) = LoadForWrite();
            if (Entries(sdks, null).FirstOrDefault(entry => SdkRegistries.SamePath(entry.Path, full)) is SdkRegistration existing)
            {
                return existing.Id;
            }

            string id = SdkRegistries.NewId();
            sdks.Add(new JsonObject { ["id"] = id, ["path"] = full });
            Save(root);
            return id;
        });
    }

    public bool Unregister(string idOrPath) => SdkRegistries.Exclusive(() =>
    {
        if (!File.Exists(file))
        {
            return false;
        }

        (JsonObject root, JsonArray sdks) = LoadForWrite();
        // 지울 항목만 고른다(읽을 수 없는 항목은 ID가 같아도 문자열 칸이 있어야 지운다).
        JsonNode?[] removed = [.. sdks.Where(node => node is JsonObject entry && Text(entry, "id") is string id && Text(entry, "path") is string entryPath &&
            (SdkRegistries.SameId(id, idOrPath) || (System.IO.Path.IsPathFullyQualified(entryPath) && SdkRegistries.SamePath(entryPath, idOrPath))))];
        foreach (JsonNode? node in removed)
        {
            sdks.Remove(node);
        }

        if (removed.Length != 0)
        {
            Save(root);
        }

        return removed.Length != 0;
    });

    // 파일을 읽는다. 깨졌거나 판을 모르면 (null, null, 이유)다.
    private (JsonObject? Root, JsonArray? Sdks, string? Problem) Load()
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(file)) is not JsonObject root)
            {
                return (null, null, $"{file}: not a JSON object");
            }

            if (root["fileVersion"] is not JsonValue version || !version.TryGetValue(out int value) || value != FileVersion)
            {
                return (null, null, $"{file}: fileVersion must be {FileVersion}");
            }

            return root["sdks"] is JsonArray sdks ? (root, sdks, null) : (null, null, $"{file}: 'sdks' must be an array");
        }
        catch (JsonException error)
        {
            return (null, null, $"{file}: {error.Message}");
        }
    }

    // 쓰기 전 읽기다. 파일이 없으면 빈 등록부다.
    private (JsonObject Root, JsonArray Sdks) LoadForWrite()
    {
        if (!File.Exists(file))
        {
            var sdks = new JsonArray();
            return (new JsonObject { ["fileVersion"] = FileVersion, ["sdks"] = sdks }, sdks);
        }

        (JsonObject? root, JsonArray? entries, string? problem) = Load();
        return root is not null && entries is not null ? (root, entries)
            : throw new InvalidDataException($"The SDK registry cannot be read ({problem}); it was not changed. Next: fix or delete {file}, then " +
                "register the SDKs again with sdk-register.");
    }

    private static List<SdkRegistration> Entries(JsonArray sdks, ICollection<string>? problems)
    {
        var entries = new List<SdkRegistration>();
        for (int index = 0; index < sdks.Count; ++index)
        {
            if (sdks[index] is not JsonObject entry || Text(entry, "id") is not { Length: > 0 } id || Text(entry, "path") is not { Length: > 0 } path)
            {
                problems?.Add($"registry entry {index}: expected {{ \"id\": string, \"path\": string }}; ignored");
            }
            else if (SdkRegistries.PathProblem(id, path) is string problem)
            {
                problems?.Add(problem);
            }
            else
            {
                entries.Add(new(id, path));
            }
        }

        return [.. entries.OrderBy(entry => entry.Id, StringComparer.Ordinal)];
    }

    private static string? Text(JsonObject entry, string name) =>
        entry[name] is JsonValue value && value.GetValueKind() == JsonValueKind.String ? value.GetValue<string>() : null;

    private void Save(JsonObject root)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(file)!);
        AtomicFile.WriteIfChanged(file, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }
}
