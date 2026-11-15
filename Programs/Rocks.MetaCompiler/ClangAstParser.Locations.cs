using System.Text.Json;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 Clang JSON 위치 색인(IndexLocations — 생략된 file 복원)과 진단 위치(UpdateSite)다.
internal sealed partial class ClangAstParser
{
    // Clang JSON은 연속 위치의 file을 생략한다. AST parent의 파일이 아니라 JSON 직렬화 순서의
    // 마지막 명시 파일을 사용한다. includedFrom은 호출자이므로 원본 파일로 취급하지 않는다.
    private void IndexLocations(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement child in element.EnumerateArray())
            {
                IndexLocations(child);
            }

            return;
        }

        if (element.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        string id = Text(element, "id");
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (property.Name == "includedFrom")
            {
                continue;
            }

            if (property.Name == "loc" && id.Length != 0)
            {
                var site = IndexLocation(property.Value);
                locations[id] = site;
            }
            else if (property.Value.ValueKind is JsonValueKind.Object or JsonValueKind.Array)
            {
                IndexLocations(property.Value);
            }
        }
        // range begin/end와 macro spelling/expansion도 파일 생략 상태를 갱신한다.
        if (element.TryGetProperty("offset", out _))
        {
            UpdateSerializedFile(element);
        }
    }

    private (string? File, int Offset) IndexLocation(JsonElement location)
    {
        if (location.ValueKind != JsonValueKind.Object)
        {
            return (null, -1);
        }

        (string? File, int Offset) site = (null, -1);
        foreach (JsonProperty property in location.EnumerateObject())
        {
            if (property.Name is "spellingLoc" or "expansionLoc")
            {
                var nested = IndexLocation(property.Value);
                if (property.Name == "expansionLoc" || site.Offset < 0)
                {
                    site = nested;
                }
            }
        }

        if (location.TryGetProperty("offset", out JsonElement offset) && offset.TryGetInt32(out int value))
        {
            UpdateSerializedFile(location);
            site = (serializedFile, value);
        }

        return site;
    }

    private void UpdateSerializedFile(JsonElement location)
    {
        string file = Text(location, "file");
        string? parent = location.TryGetProperty("includedFrom", out JsonElement included)
            ? Text(included, "file") : null;
        if (file.Length != 0)
        {
            serializedFile = file;
            serializedParent = parent;
        }
        else if (!string.Equals(serializedParent?.Replace('\\', '/'), parent?.Replace('\\', '/'),
                     OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
        {
            // 다른 include 문맥에서 파일명이 생략되면 임의로 이전 파일을 지목하지 않는다.
            serializedFile = null;
            serializedParent = null;
        }
    }

    private static SourceSite UnknownSite() => new("<unknown-location>", 1);

    private void UpdateSite(JsonElement node)
    {
        string id = Text(node, "id");
        if (!locations.TryGetValue(id, out var location) || location.File is null ||
            !File.Exists(location.File) || location.Offset < 0)
        {
            currentSite = UnknownSite();
            return;
        }

        if (!sourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(location.File);
            sourceBytes.Add(location.File, bytes);
        }

        if (location.Offset >= bytes.Length)
        {
            currentSite = UnknownSite();
            return;
        }

        int line = 1;
        for (int index = 0; index < location.Offset; ++index)
        {
            if (bytes[index] == (byte)'\n')
            {
                ++line;
            }
        }

        currentSite = new(location.File, line);
    }
}
