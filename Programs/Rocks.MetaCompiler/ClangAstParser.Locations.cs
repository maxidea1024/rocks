using System.Text.Json;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 Clang JSON 위치 색인(IndexLocations — 생략된 file 복원)과 진단 위치(UpdateSite)다.
internal sealed partial class ClangAstParser
{
    // 클래스 선언부의 원문에 모듈 API 매크로 토큰이 있는가다(Docs/Design/BuildTargets.ko.md 3.8절 BS8d3). Monolithic에서는 매크로가 빈 값이라
    // AST에 export 속성이 없으므로 원문을 본다. 범위는 선언의 시작(class·struct 키워드 — range.begin)부터 이름(loc)까지이고 주석·문자열·
    // 문자 리터럴은 빼고 토큰으로 본다. 매크로는 이 모듈(moduleName)의 것이어야 한다 — 다른 모듈 이름이면 RMC2007(Modular에서 import가
    // 되어 정의가 export되지 않는다). 매크로가 펼친 선언(범위가 매크로 자리)은 마커 검사가 따로 거부하므로 여기서는 export 없음으로 본다.
    private bool DeclaresClassExport(JsonElement node, string qualified)
    {
        string id = Text(node, "id");
        if (!locations.TryGetValue(id, out var location) || location.File is null || !File.Exists(location.File) || location.Offset < 0 ||
            !node.TryGetProperty("range", out JsonElement range) || !range.TryGetProperty("begin", out JsonElement begin) ||
            begin.TryGetProperty("expansionLoc", out _) || !begin.TryGetProperty("offset", out JsonElement beginOffset) ||
            !beginOffset.TryGetInt32(out int first))
        {
            return false;
        }

        if (!sourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(location.File);
            sourceBytes.Add(location.File, bytes);
        }

        if (first < 0 || location.Offset > bytes.Length || first > location.Offset)
        {
            return false;
        }

        // Clang offset은 byte 위치다. 선언 시작과 이름 시작은 토큰 경계라 UTF-8 문자를 쪼개지 않는다.
        string head = System.Text.Encoding.UTF8.GetString(bytes, first, location.Offset - first);
        string code = System.Text.RegularExpressions.Regex.Replace(head, @"//[^\n]*|/\*[\s\S]*?\*/|""(?:\\.|[^""\\])*""|'(?:\\.|[^'\\])*'", " ",
            System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        bool exported = false;
        foreach (System.Text.RegularExpressions.Match token in System.Text.RegularExpressions.Regex.Matches(code, @"\bROCKS_[A-Z0-9]+_API\b",
            System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)))
        {
            string own = "ROCKS_" + moduleName.ToUpperInvariant() + "_API";
            if (moduleName.Length != 0 && token.Value != own)
            {
                throw new MetadataException($"{qualified}: {token.Value} is the API macro of another module; a class of module {moduleName} is " +
                    $"exported with {own}", "RMC2007",
                    $"Use the macro of the module that defines the class: class {own} {qualified.Split("::")[^1]} (Docs/Design/BuildTargets.ko.md section 3.8 BS8d3).")
                    .At(currentSite);
            }

            exported = true;
        }

        return exported;
    }

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
