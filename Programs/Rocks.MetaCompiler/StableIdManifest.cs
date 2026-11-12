using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 과거 이름과 폐기된 ID도 영구 예약한다. 별칭은 변경 이력이지 자동 이름 복구 수단이 아니다.
internal sealed record StableIdEntry(string Kind, string Name, string Id,
    IReadOnlyList<string> Aliases, bool Retired);

internal sealed class StableIdManifest
{
    private const int MaximumBytes = BoundedFile.MaximumIdentityBytes;
    private const int MaximumEntries = 100000;
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private readonly List<StableIdEntry> entries;

    private StableIdManifest(IEnumerable<StableIdEntry> values)
    {
        entries = values.Select(Copy).ToList();
        Validate(entries);
    }

    public IReadOnlyList<StableIdEntry> Entries => entries.Select(Copy).ToArray();

    public static StableIdManifest Load(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        string full = Path.GetFullPath(path);
        if (IsLink(full))
        {
            throw new InvalidDataException("Stable ID manifest cannot be a link: " + full);
        }

        byte[] bytes = BoundedFile.Read(full, MaximumBytes);
        // 잘못된 UTF-8은 대체 문자로 복구하지 않는다.
        string json = Utf8.GetString(bytes);
        JsonDocument parsed;
        try
        {
            parsed = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 8 });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException("Invalid stable ID manifest JSON", error);
        }

        using JsonDocument doc = parsed;
        var root = Object(doc.RootElement, "manifest", ["schema", "entries"]);
        if (root["schema"].ValueKind != JsonValueKind.Number || !root["schema"].TryGetInt32(out int schema) || schema != 1)
        {
            throw new InvalidDataException("Unsupported stable ID manifest schema");
        }

        JsonElement array = root["entries"];
        if (array.ValueKind != JsonValueKind.Array || array.GetArrayLength() > MaximumEntries)
        {
            throw new InvalidDataException("Invalid stable ID entries");
        }

        List<StableIdEntry> values = [];
        foreach (JsonElement item in array.EnumerateArray())
        {
            var fields = Object(item, "entry", ["kind", "name", "id", "aliases", "retired"]);
            if (fields["kind"].ValueKind != JsonValueKind.String || fields["name"].ValueKind != JsonValueKind.String ||
                fields["id"].ValueKind != JsonValueKind.String || fields["aliases"].ValueKind != JsonValueKind.Array ||
                fields["retired"].ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            {
                throw new InvalidDataException("Invalid stable ID entry field type");
            }

            List<string> aliases = [];
            foreach (JsonElement alias in fields["aliases"].EnumerateArray())
            {
                if (alias.ValueKind != JsonValueKind.String)
                {
                    throw new InvalidDataException("Invalid stable ID alias");
                }

                aliases.Add(alias.GetString()!);
            }

            values.Add(new(fields["kind"].GetString()!, fields["name"].GetString()!, fields["id"].GetString()!,
                aliases, fields["retired"].GetBoolean()));
        }

        return new(values);
    }

    // 등록/수정은 빌드와 분리된 명시 명령에서만 호출한다. 잠금 파일은 비정상 종료 뒤에도
    // 남아 후속 쓰기를 차단한다. 사용자가 소유자를 확인하고 수동으로 해제해야 한다.
    // 외부 편집기나 Git이 같은 파일을 동시에 수정하지 않는 것은 호출자의 전제다.
    public static T Update<T>(string path, bool create, Func<StableIdManifest, T> mutation)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(mutation);
        string full = Path.GetFullPath(path);
        string parent = Path.GetDirectoryName(full) ?? throw new InvalidDataException("Manifest has no parent");
        if (!Directory.Exists(parent))
        {
            throw new DirectoryNotFoundException(parent);
        }

        string lockPath = full + ".lock";
        using var fileLock = new FileStream(lockPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        try
        {
            bool exists = File.Exists(full);
            if (IsLink(full))
            {
                throw new InvalidDataException("Stable ID manifest cannot be a link");
            }

            if (!exists && !create)
            {
                throw new FileNotFoundException("Stable ID manifest does not exist", full);
            }

            byte[]? before = exists ? BoundedFile.Read(full, MaximumBytes) : null;
            StableIdManifest manifest = exists ? Load(full) : new([]);
            byte[] originalState = manifest.Serialize();
            T result = mutation(manifest);
            Validate(manifest.entries);
            byte[] after = manifest.Serialize();
            if (after.Length > MaximumBytes)
            {
                throw new InvalidDataException("Updated stable ID manifest exceeds size limit");
            }

            if (before is null ? File.Exists(full) : !File.Exists(full) || !BoundedFile.Read(full, MaximumBytes).AsSpan().SequenceEqual(before))
            {
                throw new IOException("Stable ID manifest changed during update; retry after resolving concurrent edits");
            }

            if (before is not null && originalState.AsSpan().SequenceEqual(after))
            {
                return result;
            }

            string temporary = full + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    output.Write(after);
                    output.Flush(true);
                }
                // 같은 디렉터리에서 rename이 게시 지점이다. 실패하면 기존 파일은 유지한다.
                File.Move(temporary, full, overwrite: exists);
            }
            finally
            {
                if (File.Exists(temporary))
                {
                    File.Delete(temporary);
                }
            }

            return result;
        }
        finally
        {
            fileLock.Dispose();
            File.Delete(lockPath);
        }
    }

    public string Resolve(string kind, string name)
    {
        ValidateKindAndName(kind, name);
        StableIdEntry? entry = entries.FirstOrDefault(value => value.Kind == kind && value.Name == name);
        if (entry is null || entry.Retired)
        {
            throw new InvalidDataException($"Stable ID for {kind} {name} is missing or retired; register or explicitly rename it before building");
        }

        return entry.Id;
    }

    public string Register(string kind, string name) => Adopt(kind, name, Guid.NewGuid().ToString("N"));

    // 발급된 ID를 등록하기 전에 이름, 범위와 기존 예약 ID를 모두 검증한다.
    private string Adopt(string kind, string name, string id)
    {
        ValidateKindAndName(kind, name);
        ValidateId(id);
        var next = entries.Select(Copy).Append(new StableIdEntry(kind, name, id, [], false)).ToList();
        Validate(next);
        entries.Clear();
        entries.AddRange(next);
        return id;
    }

    public void Rename(string kind, string oldName, string newName)
    {
        ValidateKindAndName(kind, oldName);
        ValidateKindAndName(kind, newName);
        int index = entries.FindIndex(value => value.Kind == kind && value.Name == oldName && !value.Retired);
        if (index < 0)
        {
            throw new InvalidDataException($"Active {kind} {oldName} does not exist");
        }

        if (oldName == newName)
        {
            return;
        }

        if (kind == "field" && Owner(oldName) != Owner(newName))
        {
            throw new InvalidDataException("Moving fields to another type requires explicit migration");
        }

        if (kind == "type" && entries.Any(value => value.Kind == "type" && value.Name.StartsWith(oldName + "::", StringComparison.Ordinal)))
        {
            throw new InvalidDataException("Renaming a type containing reflected nested types requires explicit migration");
        }

        var next = entries.Select(Copy).ToList();
        next[index] = RenameEntry(next[index], newName);
        if (kind == "type")
        {
            for (int i = 0; i < next.Count; ++i)
            {
                if (next[i].Kind == "field" && Owner(next[i].Name) == oldName)
                {
                    next[i] = RenameEntry(next[i], newName + next[i].Name[oldName.Length..]);
                }
            }
        }

        Validate(next);
        entries.Clear();
        entries.AddRange(next);
    }

    public void Retire(string kind, string name)
    {
        ValidateKindAndName(kind, name);
        int index = entries.FindIndex(value => value.Kind == kind && value.Name == name && !value.Retired);
        if (index < 0)
        {
            throw new InvalidDataException($"Active {kind} {name} does not exist");
        }

        var next = entries.Select(Copy).ToList();
        next[index] = next[index] with { Retired = true };
        if (kind == "type")
        {
            for (int i = 0; i < next.Count; ++i)
            {
                if (next[i].Kind == "field" && Owner(next[i].Name) == name)
                {
                    next[i] = next[i] with { Retired = true };
                }
            }
        }

        Validate(next);
        entries.Clear();
        entries.AddRange(next);
    }

    private static StableIdEntry RenameEntry(StableIdEntry entry, string name) =>
        entry with { Name = name, Aliases = entry.Aliases.Append(entry.Name).ToArray() };

    private byte[] Serialize()
    {
        var document = new
        {
            schema = 1,
            entries = entries.OrderBy(value => value.Kind, StringComparer.Ordinal)
            .ThenBy(value => value.Name, StringComparer.Ordinal).Select(value => new
            {
                kind = value.Kind,
                name = value.Name,
                id = value.Id,
                aliases = value.Aliases.Order(StringComparer.Ordinal).ToArray(),
                retired = value.Retired
            }).ToArray()
        };
        return Utf8.GetBytes(JsonSerializer.Serialize(document, new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }

    private static Dictionary<string, JsonElement> Object(JsonElement node, string description, string[] required)
    {
        if (node.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException($"Expected {description} object");
        }

        Dictionary<string, JsonElement> fields = new(StringComparer.Ordinal);
        foreach (JsonProperty property in node.EnumerateObject())
        {
            if (!required.Contains(property.Name, StringComparer.Ordinal) || !fields.TryAdd(property.Name, property.Value))
            {
                throw new InvalidDataException($"Unknown or duplicate {description} property: {property.Name}");
            }
        }

        if (fields.Count != required.Length)
        {
            throw new InvalidDataException($"Missing {description} property");
        }

        return fields;
    }

    private static void Validate(IReadOnlyList<StableIdEntry> values)
    {
        if (values.Count > MaximumEntries)
        {
            throw new InvalidDataException("Too many stable ID entries");
        }

        HashSet<string> ids = new(StringComparer.Ordinal);
        HashSet<(string Kind, string Name)> names = [];
        foreach (StableIdEntry entry in values)
        {
            ValidateKindAndName(entry.Kind, entry.Name);
            ValidateId(entry.Id);
            if (!ids.Add(entry.Id))
            {
                throw new InvalidDataException("Duplicate stable ID: " + entry.Id);
            }

            if (!names.Add((entry.Kind, entry.Name)))
            {
                throw new InvalidDataException("Duplicate stable ID name: " + entry.Name);
            }

            foreach (string alias in entry.Aliases)
            {
                ValidateKindAndName(entry.Kind, alias);
                if (!names.Add((entry.Kind, alias)))
                {
                    throw new InvalidDataException("Duplicate or reused stable ID name: " + alias);
                }
            }
        }

        Dictionary<string, StableIdEntry> owners = new(StringComparer.Ordinal);
        foreach (StableIdEntry type in values.Where(value => value.Kind == "type"))
        {
            owners.Add(type.Name, type);
            foreach (string alias in type.Aliases)
            {
                owners.Add(alias, type);
            }
        }

        foreach (StableIdEntry field in values.Where(value => value.Kind == "field"))
        {
            if (!owners.TryGetValue(Owner(field.Name), out StableIdEntry? owner) ||
                (!field.Retired && (owner.Retired || owner.Name != Owner(field.Name))))
            {
                throw new InvalidDataException("Field owner is missing, retired, or not current: " + field.Name);
            }

            foreach (string alias in field.Aliases)
            {
                if (!owners.TryGetValue(Owner(alias), out StableIdEntry? prior) || prior.Id != owner.Id)
                {
                    throw new InvalidDataException("Field history changes its owning type identity: " + alias);
                }
            }
        }
    }

    private static void ValidateKindAndName(string kind, string name)
    {
        if (kind is not ("type" or "field") || string.IsNullOrEmpty(name) || name.Length > 2048)
        {
            throw new InvalidDataException("Invalid stable ID kind/name");
        }

        string[] parts = name.Split("::", StringSplitOptions.None);
        if ((kind == "field" && parts.Length < 2) || parts.Any(part => part.Length == 0 ||
            !(char.IsLetter(part[0]) || part[0] == '_') || part.Any(ch => !(char.IsLetterOrDigit(ch) || ch == '_'))))
        {
            throw new InvalidDataException("Invalid qualified declaration name: " + name);
        }
    }

    private static string Owner(string name) => name[..name.LastIndexOf("::", StringComparison.Ordinal)];
    private static void ValidateId(string id)
    {
        if (id is null || id.Length != 32 || !Guid.TryParseExact(id, "N", out Guid guid) || guid == Guid.Empty ||
            id.Any(ch => !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))))
        {
            throw new InvalidDataException("Stable ID must be a nonzero lowercase 32-digit GUID");
        }
    }

    private static bool IsLink(string path) => File.Exists(path) && new FileInfo(path).LinkTarget is not null;
    private static StableIdEntry Copy(StableIdEntry value) => value with { Aliases = value.Aliases.ToArray() };
}
