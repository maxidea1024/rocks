using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.ToolTests;

// Sparks VS Code 확장(Docs/Design/SparksTools.ko.md 12절 LC8g)의 패키징 검사: manifest 필드, 정확한 의존 판과 lock 파일, 문법의
// include·정규식, 빌드 산출물 무시. 문법 키워드와 컴파일러 키워드의 대조는 C++ 시험(SparksToolsVSCodeTests)이 한다.
internal static class SparksVSCodeTests
{
    private static readonly string[] ExtensionDirectory = ["Tools", "VSCode", "sparks"];

    private static readonly Regex ExactVersion = new("^[0-9]+\\.[0-9]+\\.[0-9]+$", RegexOptions.CultureInvariant);

    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("SparksVSCode.ManifestContributions", () => ManifestContributions(options.Root));
        suite.Add("SparksVSCode.DependenciesArePinned", () => DependenciesArePinned(options.Root));
        suite.Add("SparksVSCode.GrammarIsWellFormed", () => GrammarIsWellFormed(options.Root));
        suite.Add("SparksVSCode.BuildOutputIsIgnored", () => BuildOutputIsIgnored(options.Root));
    }

    private static string ExtensionPath(string root, params string[] parts) => Path.Combine([root, .. ExtensionDirectory, .. parts]);

    private static JsonElement ReadJson(string path)
    {
        Verify.That(File.Exists(path), $"{path} exists");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        return document.RootElement.Clone();
    }

    private static string Text(JsonElement element, string name)
    {
        Verify.That(element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String, $"'{name}' is a string");
        return value.GetString()!;
    }

    private static string[] Strings(JsonElement element)
    {
        Verify.That(element.ValueKind == JsonValueKind.Array, "value is an array");
        return element.EnumerateArray().Select(item => item.GetString() ?? "").ToArray();
    }

    private static JsonElement Single(JsonElement array, string what)
    {
        Verify.That(array.ValueKind == JsonValueKind.Array && array.GetArrayLength() == 1, $"exactly one {what}");
        return array[0];
    }

    private static void ManifestContributions(string root)
    {
        JsonElement manifest = ReadJson(ExtensionPath(root, "package.json"));
        Verify.Equal("sparks", Text(manifest, "name"), "extension name");
        Verify.Equal("Sparks", Text(manifest, "displayName"), "display name");
        Verify.Equal("rocks", Text(manifest, "publisher"), "publisher");
        Verify.Equal("UNLICENSED", Text(manifest, "license"), "license (not published)");
        Verify.That(manifest.GetProperty("private").GetBoolean(), "the extension is private (not published)");
        Verify.Equal("^1.91.0", Text(manifest.GetProperty("engines"), "vscode"), "VS Code engine floor");
        Verify.Equal("^" + Text(manifest.GetProperty("devDependencies"), "@types/vscode"), Text(manifest.GetProperty("engines"), "vscode"),
            "@types/vscode matches the engine floor");
        string[] activation = Strings(manifest.GetProperty("activationEvents"));
        Verify.That(activation.Contains("onLanguage:sparks") && activation.Contains("workspaceContains:**/SparksPackage.json5"), "activation events");

        // main은 tsconfig의 outDir 아래 src/extension.ts의 출력이다.
        JsonElement tsconfig = ReadJson(ExtensionPath(root, "tsconfig.json")).GetProperty("compilerOptions");
        Verify.Equal("./" + Text(tsconfig, "outDir") + "/extension.js", Text(manifest, "main"), "main is the compiled entry point");
        Verify.That(File.Exists(ExtensionPath(root, Text(tsconfig, "rootDir"), "extension.ts")), "the entry point source exists");

        JsonElement contributes = manifest.GetProperty("contributes");
        JsonElement language = Single(contributes.GetProperty("languages"), "language");
        Verify.Equal("sparks", Text(language, "id"), "language id");
        Verify.That(Strings(language.GetProperty("aliases")).SequenceEqual(["Sparks"]), "language alias");
        Verify.That(Strings(language.GetProperty("extensions")).SequenceEqual([".spark"]), "file extension");
        Verify.That(File.Exists(ExtensionPath(root, Text(language, "configuration"))), "language configuration exists");
        ReadJson(ExtensionPath(root, Text(language, "configuration")));

        JsonElement grammar = Single(contributes.GetProperty("grammars"), "grammar");
        Verify.Equal("sparks", Text(grammar, "language"), "grammar language");
        Verify.Equal("source.sparks", Text(grammar, "scopeName"), "grammar scope");
        Verify.Equal("source.sparks", Text(ReadJson(ExtensionPath(root, Text(grammar, "path"))), "scopeName"), "grammar file scope");

        JsonElement properties = contributes.GetProperty("configuration").GetProperty("properties");
        JsonElement serverPath = properties.GetProperty("sparks.server.path");
        Verify.Equal("string", Text(serverPath, "type"), "server path type");
        Verify.Equal("", serverPath.GetProperty("default").GetString(), "server path default (automatic lookup)");
        JsonElement schemas = properties.GetProperty("sparks.server.schemas");
        Verify.Equal("array", Text(schemas, "type"), "schema folders type");
        Verify.Equal("string", Text(schemas.GetProperty("items"), "type"), "schema folder item type");
        Verify.Equal(0, schemas.GetProperty("default").GetArrayLength(), "schema folders default");
        JsonElement trace = properties.GetProperty("sparks.trace.server");
        Verify.That(Strings(trace.GetProperty("enum")).SequenceEqual(["off", "messages", "verbose"]), "trace levels");
        Verify.Equal("off", trace.GetProperty("default").GetString(), "trace default");
        Verify.Equal(3, properties.EnumerateObject().Count(), "no other settings");

        JsonElement command = Single(contributes.GetProperty("commands"), "command");
        Verify.Equal("sparks.restartServer", Text(command, "command"), "restart command");
        Verify.Equal("Restart Language Server", Text(command, "title"), "restart command title");
        Verify.Equal("Sparks", Text(command, "category"), "restart command category");

        JsonElement defaults = contributes.GetProperty("configurationDefaults").GetProperty("[sparks]");
        Verify.That(defaults.GetProperty("editor.insertSpaces").GetBoolean(), "spaces (official format)");
        Verify.Equal(4, defaults.GetProperty("editor.tabSize").GetInt32(), "indent width (official format)");
        Verify.That(defaults.GetProperty("editor.semanticHighlighting.enabled").GetBoolean(), "semantic highlighting on");
    }

    private static void DependenciesArePinned(string root)
    {
        JsonElement manifest = ReadJson(ExtensionPath(root, "package.json"));
        JsonElement lockFile = ReadJson(ExtensionPath(root, "package-lock.json"));
        Verify.Equal(3, lockFile.GetProperty("lockfileVersion").GetInt32(), "lock file version");
        JsonElement packages = lockFile.GetProperty("packages");
        JsonElement lockRoot = packages.GetProperty("");
        foreach (string section in new[] { "dependencies", "devDependencies" })
        {
            JsonElement declared = manifest.GetProperty(section);
            JsonElement locked = lockRoot.GetProperty(section);
            Verify.Equal(declared.EnumerateObject().Count(), locked.EnumerateObject().Count(), $"lock root {section} count");
            foreach (JsonProperty dependency in declared.EnumerateObject())
            {
                string version = dependency.Value.GetString()!;
                Verify.That(ExactVersion.IsMatch(version), $"{dependency.Name} is pinned to an exact version ({version})");
                Verify.Equal(version, locked.GetProperty(dependency.Name).GetString(), $"lock root {dependency.Name}");
                Verify.Equal(version, Text(packages.GetProperty("node_modules/" + dependency.Name), "version"), $"locked {dependency.Name}");
            }
        }

        // 전이 의존까지 모든 항목이 받은 곳과 integrity를 갖는다(npm ci가 같은 byte를 받는다).
        int count = 0;
        foreach (JsonProperty entry in packages.EnumerateObject().Where(entry => entry.Name.Length > 0))
        {
            Verify.That(Text(entry.Value, "integrity").StartsWith("sha512-", StringComparison.Ordinal), $"{entry.Name} has a sha512 integrity");
            Verify.That(Text(entry.Value, "resolved").StartsWith("https://registry.npmjs.org/", StringComparison.Ordinal),
                $"{entry.Name} resolves from the npm registry");
            ++count;
        }

        Verify.That(count >= manifest.GetProperty("dependencies").EnumerateObject().Count(), "lock file lists packages");
    }

    private static void GrammarIsWellFormed(string root)
    {
        JsonElement grammar = ReadJson(ExtensionPath(root, "syntaxes", "sparks.tmLanguage.json"));
        JsonElement repository = grammar.GetProperty("repository");
        List<string> includes = [];
        List<(string Where, string Pattern)> regexes = [];
        Collect(grammar, "", includes, regexes);
        Verify.That(includes.Count > 0 && regexes.Count > 0, "grammar has includes and patterns");
        foreach (string include in includes)
        {
            Verify.That(include.StartsWith('#') && repository.TryGetProperty(include[1..], out _), $"include '{include}' names a repository rule");
        }

        // 쓰이지 않는 규칙이 없다(이름을 바꾸다 남은 규칙은 강조에서 조용히 빠진다).
        foreach (JsonProperty rule in repository.EnumerateObject())
        {
            Verify.That(includes.Contains("#" + rule.Name), $"repository rule '{rule.Name}' is included");
        }

        // VS Code의 Oniguruma와 .NET 정규식은 같지 않지만 쓰는 구문(묶음·lookahead·문자 집합·반복)은 겹친다 — 문법 실수를 잡는다.
        foreach ((string where, string pattern) in regexes)
        {
            try
            {
                _ = new Regex(pattern, RegexOptions.CultureInvariant);
            }
            catch (ArgumentException error)
            {
                Verify.That(false, $"{where}: '{pattern}' is not a regular expression: {error.Message}");
            }
        }
    }

    private static void Collect(JsonElement element, string where, List<string> includes, List<(string, string)> regexes)
    {
        if (element.ValueKind == JsonValueKind.Array)
        {
            int index = 0;
            foreach (JsonElement item in element.EnumerateArray())
            {
                Collect(item, $"{where}[{index}]", includes, regexes);
                ++index;
            }

            return;
        }

        if (element.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        foreach (JsonProperty property in element.EnumerateObject())
        {
            string path = where + "/" + property.Name;
            if (property.Name == "include")
            {
                includes.Add(property.Value.GetString()!);
            }
            else if (property.Name is "match" or "begin" or "end")
            {
                regexes.Add((path, property.Value.GetString()!));
            }
            else
            {
                Collect(property.Value, path, includes, regexes);
            }
        }
    }

    private static void BuildOutputIsIgnored(string root)
    {
        string[] lines = File.ReadAllLines(Path.Combine(root, ".gitignore"));
        Verify.That(lines.Contains("/Tools/VSCode/*/node_modules/"), "npm dependencies are ignored");
        Verify.That(lines.Contains("/Tools/VSCode/*/out/"), "compiled extension output is ignored");
    }
}
