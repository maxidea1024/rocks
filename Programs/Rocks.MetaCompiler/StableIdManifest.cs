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

    // 한 리비전의 모든 명세를 ID 기준으로 합친 뷰다(명세 위치 이동을 이력 검사가 삭제로 오인하지 않게 한다). 빈
    // 목록은 빈 명세다. 같은 ID나 같은 이름이 두 파일에 있으면 InvalidDataException이므로 먼저 SetFailures로
    // 파일과 함께 보고한다.
    public static StableIdManifest Combine(IEnumerable<StableIdManifest> manifests) =>
        new(manifests.SelectMany(manifest => manifest.entries));

    // 명세 집합(타깃·리비전의 모든 명세)에서 같은 ID, 또는 같은 종류의 같은 이름·별칭이 두 파일에 나오는 경우를
    // 파일 경로와 함께 돌려준다. 한 ID·한 이름은 한 명세에만 있다(타입을 옮길 때는 ids move). 은퇴 항목도 센다.
    public static IReadOnlyList<string> SetFailures(IEnumerable<(string Path, StableIdManifest Manifest)> manifests)
    {
        var ids = new Dictionary<string, string>(StringComparer.Ordinal);
        var names = new Dictionary<(string Kind, string Name), string>();
        var failures = new List<string>();
        foreach ((string path, StableIdManifest manifest) in manifests)
        {
            foreach (StableIdEntry entry in manifest.entries)
            {
                if (ids.TryGetValue(entry.Id, out string? first) && first != path)
                {
                    failures.Add($"ID {entry.Id} ({entry.Kind} {entry.Name}) appears in both {first} and {path}");
                }
                else
                {
                    ids[entry.Id] = path;
                }

                foreach (string name in entry.Aliases.Prepend(entry.Name))
                {
                    if (names.TryGetValue((entry.Kind, name), out string? owner) && owner != path)
                    {
                        failures.Add($"{entry.Kind} name {name} appears in both {owner} and {path}");
                    }
                    else
                    {
                        names[(entry.Kind, name)] = path;
                    }
                }
            }
        }

        return failures;
    }

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

    // 새 프로젝트용으로 ID를 다시 발급한다(Docs/Design/ProjectFiles.ko.md 11.4절): 활성 항목은 Register와 같은 생성기로 새 ID를 받고 aliases를
    // 비우며, 은퇴 항목은 지운다(새 프로젝트에는 이력이 없다). 템플릿에서 만든 프로젝트끼리 같은 타입 ID를 갖지 않게 한다. 다시 발급한 수다.
    public int Reissue()
    {
        var next = entries.Where(value => !value.Retired)
            .Select(value => new StableIdEntry(value.Kind, value.Name, Guid.NewGuid().ToString("N"), [], false)).ToList();
        Validate(next);
        entries.Clear();
        entries.AddRange(next);
        return next.Count;
    }

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

        if (IsMember(kind) && Owner(oldName) != Owner(newName))
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
                if (IsMember(next[i].Kind) && Owner(next[i].Name) == oldName)
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
                if (IsMember(next[i].Kind) && Owner(next[i].Name) == name)
                {
                    next[i] = next[i] with { Retired = true };
                }
            }
        }

        Validate(next);
        entries.Clear();
        entries.AddRange(next);
    }

    // ids move의 원본 쪽: type과 그 필드(·중첩 타입은 거부) 항목을 ID·별칭·은퇴 상태째로 떼어 돌려준다.
    public IReadOnlyList<StableIdEntry> Detach(string typeName)
    {
        ValidateKindAndName("type", typeName);
        if (!entries.Any(value => value.Kind == "type" && value.Name == typeName))
        {
            throw new InvalidDataException($"type {typeName} does not exist in this manifest");
        }

        if (entries.Any(value => value.Kind == "type" && value.Name.StartsWith(typeName + "::", StringComparison.Ordinal)))
        {
            throw new InvalidDataException("Moving a type containing reflected nested types requires moving each nested type explicitly");
        }

        var moved = entries.Where(value => (value.Kind == "type" && value.Name == typeName) ||
            (IsMember(value.Kind) && Owner(value.Name) == typeName)).Select(Copy).ToList();
        var next = entries.Where(value => !moved.Any(item => item.Kind == value.Kind && item.Name == value.Name)).Select(Copy).ToList();
        Validate(next);
        entries.Clear();
        entries.AddRange(next);
        return moved;
    }

    // ids move의 대상 쪽: 떼어 낸 항목을 그대로 붙인다. 같은 항목이 이미 모두 있으면 아무것도 하지 않는다(중단 뒤 재실행).
    public void Attach(IReadOnlyList<StableIdEntry> moved)
    {
        bool Same(StableIdEntry left, StableIdEntry right) => left.Kind == right.Kind && left.Name == right.Name &&
            left.Id == right.Id && left.Retired == right.Retired && left.Aliases.Order(StringComparer.Ordinal)
                .SequenceEqual(right.Aliases.Order(StringComparer.Ordinal), StringComparer.Ordinal);
        if (moved.All(item => entries.Any(value => Same(value, item))))
        {
            return;
        }

        var next = entries.Select(Copy).Concat(moved.Select(Copy)).ToList();
        Validate(next);
        entries.Clear();
        entries.AddRange(next);
    }

    private static StableIdEntry RenameEntry(StableIdEntry entry, string name) =>
        entry with { Name = name, Aliases = entry.Aliases.Append(entry.Name).ToArray() };

    // 저장 형식 byte다. Sparks 도구(`sparks ids`)가 같은 정규형을 쓰므로 시험이 두 쪽 출력을 같은 golden과 비교한다
    // (Docs/Design/SparksTools.ko.md 7.2절).
    internal byte[] Serialize()
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
        // 줄바꿈 기본값은 플랫폼마다 다르므로(Windows CRLF) LF로 고정한다. 저장소 명세는 LF로 관리된다.
        return Utf8.GetBytes(JsonSerializer.Serialize(document, new JsonSerializerOptions { WriteIndented = true, NewLine = "\n" }) + "\n");
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

        foreach (StableIdEntry field in values.Where(value => IsMember(value.Kind)))
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

    // 멤버 kind(이름의 주인 type이 같은 명세에 있어야 하는 것)다. C++은 field뿐이고 Sparks는 subobject·function·
    // rpc·enumCase를 더 쓴다(Docs/Design/SparksBytecode.ko.md 6.1절, SE-R8).
    internal static bool IsMember(string kind) => kind is "field" or "subobject" or "function" or "rpc" or "enumCase";

    private static bool IsIdentifier(string part) => part.Length > 0 && (char.IsAsciiLetter(part[0]) || part[0] == '_') &&
        part.All(ch => char.IsAsciiLetterOrDigit(ch) || ch == '_');

    private static void ValidateKindAndName(string kind, string name)
    {
        if (kind is not ("type" or "field" or "subobject" or "function" or "rpc" or "enumCase") || string.IsNullOrEmpty(name) ||
            name.Length > 2048)
        {
            throw new InvalidDataException("Invalid stable ID kind/name");
        }

        if (name.Contains('/'))
        {
            ValidateSparksName(kind, name);
            return;
        }

        // C++ 정규 이름(`a::b::Type`, 멤버는 `::member`)은 type·field만 쓴다.
        string[] parts = name.Split("::", StringSplitOptions.None);
        if (kind is not ("type" or "field") || (kind == "field" && parts.Length < 2) || parts.Any(part => !IsIdentifier(part)))
        {
            throw new InvalidDataException("Invalid qualified declaration name: " + name);
        }
    }

    // Sparks 이름 키: `<패키지>/<모듈 경로>.<선언>[.<멤버>]`. 패키지·경로 조각은 식별자(`-` 허용), 선언은 식별자,
    // 멤버는 식별자 또는 handler의 `on(<경로>)`(경로는 `.`로 이은 식별자·`this`)다. type은 멤버가 없고 멤버
    // kind는 멤버가 있다.
    private static void ValidateSparksName(string kind, string name)
    {
        int slash = name.LastIndexOf('/');
        int dot = name.IndexOf('.', slash + 1);
        string[] path = name[..(dot < 0 ? name.Length : dot)].Split('/');
        bool valid = dot > slash + 1 && path.Length >= 2 && path.All(part => part.Length > 0 &&
            (char.IsAsciiLetter(part[0]) || part[0] == '_') && part.All(ch => char.IsAsciiLetterOrDigit(ch) || ch is '_' or '-'));
        string rest = dot < 0 ? string.Empty : name[(dot + 1)..];
        int memberDot = rest.IndexOf('.');
        string declaration = memberDot < 0 ? rest : rest[..memberDot];
        string member = memberDot < 0 ? string.Empty : rest[(memberDot + 1)..];
        valid = valid && IsIdentifier(declaration);
        if (kind == "type")
        {
            valid = valid && member.Length == 0;
        }
        else if (member.StartsWith("on(", StringComparison.Ordinal) && member.EndsWith(')'))
        {
            valid = valid && kind == "function" && member[3..^1].Split('.').All(IsIdentifier);
        }
        else
        {
            valid = valid && IsIdentifier(member);
        }

        if (!valid)
        {
            throw new InvalidDataException("Invalid Sparks declaration name: " + name);
        }
    }

    // 멤버 이름의 주인 type 이름이다(C++은 마지막 `::` 앞, Sparks는 모듈 경로 뒤 첫 `.`의 선언까지). 병합 검사
    // (StableIdMergeValidator)도 같은 규칙을 쓴다.
    internal static string Owner(string name)
    {
        if (!name.Contains('/'))
        {
            return name[..name.LastIndexOf("::", StringComparison.Ordinal)];
        }

        int slash = name.LastIndexOf('/');
        int dot = name.IndexOf('.', slash + 1);
        int memberDot = name.IndexOf('.', dot + 1);
        return memberDot < 0 ? name : name[..memberDot];
    }

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
