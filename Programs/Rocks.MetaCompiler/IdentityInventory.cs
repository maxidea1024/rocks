using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 단일 target에 속한 모든 입력을 합쳐 유실된 선언과 중복 정의를 검사한다.
internal static class IdentityInventory
{
    public static void Verify(string manifestPath, IReadOnlyList<string> paths)
    {
        if (paths.Count == 0)
        {
            throw new InvalidDataException("No metadata inventories were supplied.");
        }

        var manifest = StableIdManifest.Load(manifestPath);
        Dictionary<(string Kind, string Name), (string Path, int Line)> seen = [];
        int total = 0;
        foreach (string path in paths)
        {
            byte[] bytes = BoundedFile.Read(path, BoundedFile.MaximumIdentityBytes);
            using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 8 });
            var root = Fields(document.RootElement, ["schema", "declarations"]);
            if (root["schema"].ValueKind != JsonValueKind.Number || !root["schema"].TryGetInt32(out int schema) || schema != 1 ||
                root["declarations"].ValueKind != JsonValueKind.Array)
            {
                throw new InvalidDataException("Invalid identity inventory: " + path);
            }

            foreach (JsonElement item in root["declarations"].EnumerateArray())
            {
                if (++total > 1000000)
                {
                    throw new InvalidDataException("Too many identity inventory declarations.");
                }

                var fields = Fields(item, ["kind", "name", "id", "file", "line"]);
                if (fields["kind"].ValueKind != JsonValueKind.String || fields["name"].ValueKind != JsonValueKind.String ||
                    fields["id"].ValueKind != JsonValueKind.String || fields["file"].ValueKind != JsonValueKind.String ||
                    fields["line"].ValueKind != JsonValueKind.Number || !fields["line"].TryGetInt32(out int line) || line < 1)
                {
                    throw new InvalidDataException("Invalid identity inventory declaration: " + path);
                }

                string kind = fields["kind"].GetString()!, name = fields["name"].GetString()!;
                string id = fields["id"].GetString()!, file = fields["file"].GetString()!;
                if (!Path.IsPathFullyQualified(file) || !File.Exists(file) || manifest.Resolve(kind, name) != id)
                {
                    throw new InvalidDataException($"Invalid identity or source location for {kind} {name} in {path}.");
                }

                file = CanonicalPath.Resolve(file);
                var key = (kind, name);
                if (seen.TryGetValue(key, out var previous))
                {
                    throw new InvalidDataException($"Repeated reflected declaration across metadata units is not supported: {kind} {name} at {previous.Path}:{previous.Line} and {file}:{line}. Use one metadata owner until schema equivalence is validated.");
                }

                seen.Add(key, (file, line));
            }
        }

        foreach (var entry in manifest.Entries.Where(value => !value.Retired))
        {
            if (!seen.ContainsKey((entry.Kind, entry.Name)))
            {
                throw new InvalidDataException($"Active {entry.Kind} {entry.Name} is missing from target sources. Use ids rename for a rename or ids retire for a deliberate removal; do not issue a replacement ID.");
            }
        }
    }

    private static Dictionary<string, JsonElement> Fields(JsonElement node, string[] allowed)
    {
        if (node.ValueKind != JsonValueKind.Object)
        {
            throw new InvalidDataException("Identity inventory must contain objects.");
        }

        Dictionary<string, JsonElement> result = new(StringComparer.Ordinal);
        foreach (JsonProperty property in node.EnumerateObject())
        {
            if (!allowed.Contains(property.Name, StringComparer.Ordinal) || !result.TryAdd(property.Name, property.Value))
            {
                throw new InvalidDataException("Unknown or duplicate identity inventory property: " + property.Name);
            }
        }

        if (result.Count != allowed.Length)
        {
            throw new InvalidDataException("Identity inventory property is missing.");
        }

        return result;
    }
}
