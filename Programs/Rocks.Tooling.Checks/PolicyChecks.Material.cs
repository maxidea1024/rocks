using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>머티리얼 노드 목록의 단일 원본(Docs/Design/MaterialSystem 19.2.2절)이다.</summary>
    private const string MaterialNodeCatalogPath = "Engine/Source/Runtime/MaterialSchema/MaterialNodeCatalog.json";

    /// <summary>19장 문서 폴더다(선행 표 README와 노드 표 둘).</summary>
    private const string MaterialNodeCatalogDocs = "Docs/Design/MaterialSystem/19-NodeCatalog";

    /// <summary>TODO 표기 규칙(19.2.1절 결정 MD12)을 거는 머티리얼 층 모듈이다.</summary>
    public static readonly IReadOnlyList<string> MaterialTodoModules =
    [
        "Engine/Source/Runtime/Material",
        "Engine/Source/Runtime/MaterialSchema",
        "Engine/Source/Developer/MaterialCompiler",
        "Engine/Source/Runtime/RenderData",
    ];

    // 선행 표(19.4.1절)의 행이다: | `MPQ-<이름>` | …
    private static readonly Regex MaterialPrerequisiteRow = Pattern("^\\|\\s*`(MPQ-[A-Za-z0-9]+)`\\s*\\|");

    // 코드의 TODO 표기다. 형식은 TODO(MPQ-<이름>) 하나다. 주석 첫머리의 TODO(// TODO fix)와 괄호·쌍점이 붙은 TODO(TODO(·TODO (·TODO:)는
    // 모두 표기로 본다. 문장 가운데의 낱말 TODO("상태가 TODO 노드" — 상태 이름)는 산문이라 보지 않는다.
    private static readonly Regex MaterialTodoAny = Pattern("(?<=(?://|/\\*)\\s*)TODO\\b|\\bTODO(?=\\s*[(:])");
    private static readonly Regex MaterialTodoValid = Pattern("^TODO\\((MPQ-[A-Za-z0-9]+)\\)");

    // 노드 표 행이다(첫 칸이 UE 클래스).
    private static readonly Regex MaterialTableRow = Pattern("^\\|\\s*`(U\\w+)`\\s*\\|");

    // 상태 칸 표기(구현·TODO·플러그인·추상·공통·제외)다. 소스 문자열은 영어 규칙(source-literal-english)이라 한글은 이스케이프로 쓴다.
    private static readonly Dictionary<string, string> MaterialStatusLabels = new(StringComparer.Ordinal)
    {
        ["\uAD6C\uD604"] = "Implemented",
        ["TODO"] = "PrerequisiteMissing",
        ["\uD50C\uB7EC\uADF8\uC778"] = "Plugin",
        ["\uCD94\uC0C1"] = "Abstract",
        ["\uACF5\uD1B5"] = "CommonGraph",
        ["\uC81C\uC678"] = "Excluded",
    };

    /// <summary>
    /// 머티리얼 노드 목록 대조다(rule id material-catalog·material-todo·material-prerequisite, Docs/Design/MaterialSystem
    /// 19.2.1·19.2.2절). (1) 19.6절 노드 표의 (UE 클래스, 노드, 상태, 단계, 공급·선행) 열이 목록 파일과 같고 목록의 행이 표에 한 번씩
    /// 있다(플러그인 표는 클래스·노드, 노드가 아닌 클래스 표는 클래스·상태). 목록 파일의 UE 클래스는 한 번씩이다. (2) 머티리얼 층 모듈 소스(시험
    /// 포함)의 <c>TODO(</c>·<c>TODO:</c> 표기가
    /// <c>TODO(MPQ-&lt;이름&gt;)</c>이고 이름이 19.4.1절 표에 있다. (3) 표의 MPQ ID마다 코드 표기나 목록 파일의 선행(노드·속성)이 하나
    /// 이상 있다. 목록 파일이나 문서 폴더가 없으면 검사하지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> MaterialNodeCatalog(string root)
    {
        List<CheckFailure> failures = [];
        string catalogPath = Path.Combine(root, MaterialNodeCatalogPath);
        string docs = Path.Combine(root, MaterialNodeCatalogDocs);
        if (!File.Exists(catalogPath) || !Directory.Exists(docs))
        {
            return failures;
        }

        using JsonDocument catalog = JsonDocument.Parse(File.ReadAllText(catalogPath, StrictUtf8));
        var entries = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (JsonElement entry in catalog.RootElement.GetProperty("entries").EnumerateArray())
        {
            string unrealClass = entry.GetProperty("unrealClass").GetString()!;
            if (!entries.TryAdd(unrealClass, entry))
            {
                failures.Add(new(MaterialNodeCatalogPath, 1, "material-catalog", $"'{unrealClass}' has more than one catalog entry"));
            }
        }

        // (1) 노드 표.
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (string document in new[] { "NodeTableCore.ko.md", "NodeTableRendering.ko.md" })
        {
            string path = Path.Combine(docs, document);
            if (!File.Exists(path))
            {
                failures.Add(new(Relative(root, path), 1, "material-catalog", "node table document is missing"));
                continue;
            }

            string relative = Relative(root, path);
            string header = "";
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                if (text.StartsWith("| UE 5.8 |", StringComparison.Ordinal))
                {
                    header = text;
                    continue;
                }

                Match row = MaterialTableRow.Match(text);
                if (!row.Success)
                {
                    continue;
                }

                string unrealClass = row.Groups[1].Value;
                if (!entries.TryGetValue(unrealClass, out JsonElement entry))
                {
                    failures.Add(new(relative, line, "material-catalog", $"'{unrealClass}' is not in {MaterialNodeCatalogPath}"));
                    continue;
                }

                if (!seen.Add(unrealClass))
                {
                    failures.Add(new(relative, line, "material-catalog", $"'{unrealClass}' appears more than once in the node tables"));
                    continue;
                }

                string[] cells = text.Split('|').Select(cell => cell.Trim()).ToArray();
                string mismatch = MaterialRowMismatch(header, cells, entry);
                if (mismatch.Length != 0)
                {
                    failures.Add(new(relative, line, "material-catalog", $"'{unrealClass}' differs from the catalog file: {mismatch}"));
                }
            }
        }

        foreach (string unrealClass in entries.Keys.Where(name => !seen.Contains(name)))
        {
            failures.Add(new(MaterialNodeCatalogPath, 1, "material-catalog", $"'{unrealClass}' has no row in the node tables (section 19.6)"));
        }

        // (2) TODO 표기와 (3) MPQ ID 사용.
        string readme = Path.Combine(docs, "README.ko.md");
        var prerequisites = new HashSet<string>(StringComparer.Ordinal);
        if (File.Exists(readme))
        {
            foreach (string text in File.ReadLines(readme, StrictUtf8))
            {
                Match row = MaterialPrerequisiteRow.Match(text);
                if (row.Success)
                {
                    prerequisites.Add(row.Groups[1].Value);
                }
            }
        }

        var used = new HashSet<string>(StringComparer.Ordinal);
        foreach (JsonElement entry in entries.Values)
        {
            string prerequisite = entry.GetProperty("prerequisite").GetString() ?? "";
            if (prerequisite.Length != 0)
            {
                used.Add(prerequisite);
            }

            foreach (JsonElement property in entry.GetProperty("propertyPrerequisites").EnumerateArray())
            {
                used.Add(property.GetString()!);
            }
        }

        foreach (string path in Files(root, MaterialTodoModules.Where(folder => Directory.Exists(Path.Combine(root, folder))).ToList()))
        {
            string relative = Relative(root, path);
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                foreach (Match match in MaterialTodoAny.Matches(text))
                {
                    Match valid = MaterialTodoValid.Match(text[match.Index..]);
                    if (!valid.Success)
                    {
                        failures.Add(new(relative, line, "material-todo", "TODO notes in the material layer use the form TODO(MPQ-<name>) (section 19.2.1)"));
                        continue;
                    }

                    string id = valid.Groups[1].Value;
                    if (!prerequisites.Contains(id))
                    {
                        failures.Add(new(relative, line, "material-todo", $"'{id}' is not in the prerequisite table (section 19.4.1)"));
                    }

                    used.Add(id);
                }
            }
        }

        foreach (string id in prerequisites.Where(id => !used.Contains(id)).Order(StringComparer.Ordinal))
        {
            failures.Add(new(Relative(root, readme), 1, "material-prerequisite",
                $"'{id}' is neither noted in material code nor named by a catalog entry (section 19.2.2)"));
        }

        return failures;
    }

    // 표 머리에 따라 행 칸을 목록 항목과 견준다. 다르면 무엇이 다른지, 같으면 빈 문자열이다.
    private static string MaterialRowMismatch(string header, string[] cells, JsonElement entry)
    {
        string node = entry.GetProperty("node").GetString() ?? "";
        string status = entry.GetProperty("status").GetString() ?? "";
        static string Unquote(string cell) => cell.Trim('`');
        // 머리 "| 상태 | 단계 |"(본 표)다.
        if (header.Contains("| \uC0C1\uD0DC | \uB2E8\uACC4 |", StringComparison.Ordinal))
        {
            // | UE | 노드 | 상태 | 단계 | 공급·선행 | 비고 |
            if (cells.Length < 7)
            {
                return "row has fewer cells than the table header";
            }

            if (Unquote(cells[2]) != node)
            {
                return $"node '{Unquote(cells[2])}' (catalog '{node}')";
            }

            if (!MaterialStatusLabels.TryGetValue(cells[3], out string? label) || label != status)
            {
                return $"status '{cells[3]}' (catalog '{status}')";
            }

            string stage = entry.GetProperty("stage").GetString() ?? "";
            if (cells[4] != stage)
            {
                return $"stage '{cells[4]}' (catalog '{stage}')";
            }

            string supply = MaterialSupplyCell(entry);
            return cells[5] == supply ? "" : $"supply '{cells[5]}' (catalog '{supply}')";
        }

        // 머리 "| 등록 모듈 |"(플러그인 표)다.
        if (header.Contains("| \uB4F1\uB85D \uBAA8\uB4C8 |", StringComparison.Ordinal))
        {
            // | UE | 노드(가칭) | 등록 모듈 | 비고 |
            if (status != "Plugin")
            {
                return $"listed with the plugin nodes (catalog '{status}')";
            }

            return Unquote(cells[2]) == node ? "" : $"node '{Unquote(cells[2])}' (catalog '{node}')";
        }

        // | UE | 상태 | Rocks | 이유 |
        if (cells.Length < 4 || !MaterialStatusLabels.TryGetValue(cells[2], out string? other) || other != status)
        {
            return $"status '{(cells.Length > 2 ? cells[2] : "")}' (catalog '{status}')";
        }

        return "";
    }

    // 목록 항목의 "공급·선행" 칸 표기다: 공급 기능·원본 interface·선행을 각각 `…`로, 속성 선행은 `속성 TODO: A·B` 하나로 두고
    // 가운뎃점으로 잇는다. 없으면 —다.
    private static string MaterialSupplyCell(JsonElement entry)
    {
        List<string> items = [];
        foreach (string field in new[] { "supply", "source", "prerequisite" })
        {
            string value = entry.GetProperty(field).GetString() ?? "";
            if (value.Length != 0)
            {
                items.Add($"`{value}`");
            }
        }

        string[] properties = entry.GetProperty("propertyPrerequisites").EnumerateArray().Select(item => item.GetString()!).ToArray();
        if (properties.Length != 0)
        {
            items.Add($"`\uC18D\uC131 TODO: {string.Join('\u00B7', properties)}`");
        }

        return items.Count == 0 ? "\u2014" : string.Join('\u00B7', items);
    }
}
