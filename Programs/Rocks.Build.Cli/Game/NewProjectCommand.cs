using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>템플릿 정의(<c>Template.json</c>, Docs/Design/ProjectFiles.ko.md 11.2절)다. Ignore는 템플릿 루트 기준 경로('/' 구분)다.</summary>
internal sealed record ProjectTemplate(string Directory, string TemplateName, string Description, IReadOnlyList<string> Ignore,
    IReadOnlyList<string> ReplaceInExtensions);

/// <summary>
/// <c>new-project --template &lt;템플릿 디렉터리 이름|경로&gt; --name &lt;이름&gt; --destination &lt;부모 디렉터리&gt; [--sdk &lt;SDK&gt;]
/// [--projectfiles on|off] [--compiler &lt;clang-cl&gt;]</c>: 템플릿에서 새 게임 프로젝트를 만든다(Docs/Design/ProjectFiles.ko.md 11절).
/// <list type="number">
/// <item>템플릿: 값에 경로 구분자가 있으면 그 디렉터리, 아니면 SDK의 <c>Templates/Game/&lt;값&gt;</c>. SDK는 <c>--sdk</c>, 없으면 이 CLI가 든 SDK다.</item>
/// <item><c>&lt;부모&gt;/&lt;이름&gt;</c>(없거나 빈 디렉터리)에 ignore를 뺀 파일을 이름 치환하며 복사한다. 실패하면 만든 것을 지운다.</item>
/// <item>기술자의 engineVersion을 SDK 주·부로 맞추고 engineAssociation을 지운다. identity 명세는 SDK MetaCompiler <c>ids reissue</c>로 새 ID를 받는다.</item>
/// <item>결과의 경로·내용에 템플릿 이름 세 철자가 남으면 오류다. Windows에서 <c>--projectfiles on</c>(기본)이면 게임 프로젝트 파일을 만든다.</item>
/// </list>
/// </summary>
internal static class NewProjectCommand
{
    public const string Name = "new-project";

    /// <summary>템플릿 정의 파일 이름이다.</summary>
    public const string TemplateFile = "Template.json";

    // 이 명령만 쓰는 옵션이다(공통 파서가 받는 이름 목록에 더한다).
    internal static readonly string[] Options = ["template", "name", "projectfiles"];

    private const int TemplateFileVersion = 1;

    private const int MinimumTemplateNameLength = 8;

    private const int MinimumNameLength = 3;

    private const int MaximumNameLength = 32;

    // 엔진이 쓰는 이름이다(대소문자 무시). RocksSdk는 게임 솔루션의 SDK 헤더 프로젝트 이름이다(10.4절).
    private static readonly string[] ReservedNames = ["Rocks", "RocksSdk", "Engine", "Core", "Std", "Posix"];

    // Windows 예약 장치 이름이다(대소문자 무시) — 파일·디렉터리 이름이 될 수 없다(UE Paths.cpp·FileHelper.cpp와 같다).
    private static readonly HashSet<string> DeviceNames = new(["CON", "PRN", "AUX", "NUL", .. Enumerable.Range(0, 10).Select(index => "COM" + index),
        .. Enumerable.Range(0, 10).Select(index => "LPT" + index)], StringComparer.OrdinalIgnoreCase);

    // 소문자 형(namespace)이 될 수 없는 매크로·전역 이름이다: 표준 C(errno·stdin·stdout·stderr), Windows 헤더(near·far·interface),
    // GNU 사전 정의(linux·unix). 매크로로 바뀌면 생성 코드가 깨진다.
    private static readonly HashSet<string> MacroNames = new(["errno", "stdin", "stdout", "stderr", "near", "far", "interface", "linux", "unix", "small", "hyper",
        "pascal", "cdecl"],
        StringComparer.Ordinal);

    // 소문자 형이 namespace가 되므로 C++ 키워드와 같으면 안 된다(C++23 키워드와 대체 토큰).
    private static readonly HashSet<string> CppKeywords = new(StringComparer.Ordinal)
    {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
        "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
        "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
        "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };

    private static readonly Regex Identifier = new("^[A-Z][A-Za-z0-9_]*$", RegexOptions.CultureInvariant);

    public static async Task<int> RunAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        var allowed = new HashSet<string>(["template", "name", "destination", "sdk", "projectfiles", "compiler"], StringComparer.Ordinal);
        foreach (string option in options.Keys.Where(key => !allowed.Contains(key)))
        {
            throw new ArgumentException($"Option --{option} is not supported by '{Name}'.");
        }

        string templateValue = Required(options, "template");
        string name = Required(options, "name");
        string parent = Path.GetFullPath(Required(options, "destination"));
        bool projectFiles = options.GetValueOrDefault("projectfiles", "on") switch
        {
            "on" => true,
            "off" => false,
            _ => throw new ArgumentException("projectfiles must be on or off.")
        };
        string sdk = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(options.GetValueOrDefault("sdk") ?? OwnSdk())));
        if (!File.Exists(Path.Combine(sdk, SdkAssociation.ManifestName)))
        {
            throw new ArgumentException($"{sdk} is not an installed SDK ({SdkAssociation.ManifestName} not found).");
        }

        ProjectTemplate template = ReadTemplate(templateValue.Contains('/') || templateValue.Contains('\\')
            ? Path.GetFullPath(templateValue) : Path.Combine(sdk, "Templates", "Game", templateValue));
        CheckTemplate(template);
        ValidateName(name, InstalledSdk.ManifestModuleNames(sdk));
        RequireDistinctFromTemplate(name, template.TemplateName);
        string destination = Path.Combine(parent, name);
        if (File.Exists(destination) || (Directory.Exists(destination) && Directory.EnumerateFileSystemEntries(destination).Any()))
        {
            throw new ArgumentException($"{destination} already exists and is not empty; nothing was written.");
        }

        EngineVersion engine = InstalledSdk.EngineVersionOf(JsonSerializer.Deserialize<SdkManifest>(File.ReadAllText(Path.Combine(sdk, SdkAssociation.ManifestName)))
            ?? throw new InvalidDataException("Empty SDK manifest."));
        bool existed = Directory.Exists(destination);
        try
        {
            Copy(template, destination, name);
            string[] descriptors = Directory.GetFiles(destination, "*" + Rocks.Build.Descriptors.DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
            if (descriptors.Length != 1)
            {
                throw new InvalidDataException($"The template {template.Directory} must have exactly one project descriptor at its root.");
            }

            WriteDescriptor(descriptors[0], engine);
            foreach (string manifest in Directory.GetFiles(destination, "*.rocksids.json", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            {
                await ReissueAsync(sdk, manifest, cancellationToken);
            }

            if (Leftovers(destination, template).FirstOrDefault() is string leftover)
            {
                throw new InvalidDataException($"The new project still names the template ({leftover}); the template has a spelling the three rules do not cover.");
            }
        }
        catch
        {
            // 만든 것만 지운다: 있던 빈 디렉터리는 비우고 남긴다.
            if (Directory.Exists(destination))
            {
                if (existed)
                {
                    foreach (string entry in Directory.EnumerateFileSystemEntries(destination))
                    {
                        if (Directory.Exists(entry))
                        {
                            Directory.Delete(entry, recursive: true);
                        }
                        else
                        {
                            File.Delete(entry);
                        }
                    }
                }
                else
                {
                    Directory.Delete(destination, recursive: true);
                }
            }

            throw;
        }

        Console.WriteLine($"Created {destination} from the {template.TemplateName} template (engine {engine.Major}.{engine.Minor}, SDK {sdk}).");
        if (!projectFiles || !OperatingSystem.IsWindows())
        {
            return 0;
        }

        var generation = new Dictionary<string, string>(StringComparer.Ordinal) { ["project"] = destination, ["sdk"] = sdk };
        if (options.TryGetValue("compiler", out string? compiler))
        {
            generation["compiler"] = compiler;
        }

        return await GameProjectFiles.RunAsync(generation, cancellationToken);
    }

    /// <summary><c>Template.json</c>을 엄격하게 읽는다(모르는 필드·빠진 필드·형식 오류는 오류).</summary>
    internal static ProjectTemplate ReadTemplate(string directory)
    {
        string path = Path.Combine(directory, TemplateFile);
        if (!File.Exists(path))
        {
            throw new ArgumentException($"{directory} is not a project template ({TemplateFile} not found).");
        }

        JsonObject root = JsonNode.Parse(File.ReadAllText(path), documentOptions: new JsonDocumentOptions { AllowDuplicateProperties = false }) as JsonObject
            ?? throw new InvalidDataException(path + ": not a JSON object.");
        string[] known = ["fileVersion", "templateName", "description", "ignore", "replaceInExtensions"];
        if (root.Select(pair => pair.Key).FirstOrDefault(key => !known.Contains(key, StringComparer.Ordinal)) is string unknown)
        {
            throw new InvalidDataException($"{path}: unknown field '{unknown}'.");
        }

        string Text(string field) => root[field] is JsonValue value && value.GetValueKind() == JsonValueKind.String ? value.GetValue<string>()
            : throw new InvalidDataException($"{path}: '{field}' must be a string.");
        string[] List(string field) => root[field] is JsonArray array && array.All(item => item is JsonValue value && value.GetValueKind() == JsonValueKind.String)
            ? [.. array.Select(item => item!.GetValue<string>())] : throw new InvalidDataException($"{path}: '{field}' must be an array of strings.");
        if (root["fileVersion"] is not JsonValue version || !version.TryGetValue(out int number) || number != TemplateFileVersion)
        {
            throw new InvalidDataException($"{path}: fileVersion must be {TemplateFileVersion}.");
        }

        string templateName = Text("templateName");
        if (!Identifier.IsMatch(templateName) || templateName.Length < MinimumTemplateNameLength)
        {
            throw new InvalidDataException($"{path}: templateName '{templateName}' must be a C++ identifier that starts with an uppercase letter and has at least " +
                $"{MinimumTemplateNameLength} characters.");
        }

        return new(Path.GetFullPath(directory), templateName, Text("description"), [.. List("ignore").Select(entry => entry.Replace('\\', '/'))],
            List("replaceInExtensions"));
    }

    /// <summary>
    /// 템플릿 검사: 파일 이름과 치환 대상 파일 내용에서 템플릿 이름이 대소문자 무시로 나오는 곳은 모두 세 철자(그대로·대문자·소문자) 중 하나여야
    /// 한다(철자가 섞여 일부만 바뀌는 것을 막는다, 11.2절).
    /// </summary>
    internal static void CheckTemplate(ProjectTemplate template)
    {
        string[] spellings = Spellings(template.TemplateName);
        var any = new Regex(Regex.Escape(template.TemplateName), RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        foreach ((string relative, string file) in Files(template))
        {
            IEnumerable<string> texts = [relative];
            if (Replaces(template, file))
            {
                texts = texts.Append(ReadText(file).Text);
            }

            foreach (string text in texts)
            {
                if (any.Matches(text).Select(match => match.Value).FirstOrDefault(value => !spellings.Contains(value, StringComparer.Ordinal)) is string mixed)
                {
                    throw new InvalidDataException($"Template {template.Directory}: {relative} spells the template name as '{mixed}'; use {string.Join(", ", spellings)}.");
                }
            }
        }
    }

    /// <summary>
    /// 프로젝트 이름 검사(11.3절): 대문자로 시작하는 C++ 식별자, 3–32자, SDK 모듈·엔진 예약 이름과 대소문자 무시로 다름. 소문자 형은 namespace라
    /// C++ 키워드·<c>std</c>·<c>rocks</c>·<c>posix</c>가 아니어야 하고, 대문자 형은 엔진 매크로 접두 <c>ROCKS</c>가 아니어야 한다.
    /// </summary>
    internal static void ValidateName(string name, IEnumerable<string> sdkModules)
    {
        if (!Identifier.IsMatch(name) || name.Length < MinimumNameLength || name.Length > MaximumNameLength)
        {
            throw new ArgumentException($"Project name '{name}' must be a C++ identifier that starts with an uppercase letter, {MinimumNameLength}-{MaximumNameLength} characters.");
        }

        if (DeviceNames.Contains(name))
        {
            throw new ArgumentException($"Project name '{name}' is a reserved Windows device name and cannot name a file or directory.");
        }

        if (ReservedNames.Concat(sdkModules).FirstOrDefault(reserved => string.Equals(reserved, name, StringComparison.OrdinalIgnoreCase)) is string taken)
        {
            throw new ArgumentException($"Project name '{name}' is the name of an engine or SDK module ({taken}); choose another name.");
        }

        string lower = name.ToLowerInvariant();
        // 대문자 형 ROCKS(엔진 매크로 접두)는 소문자 형 rocks와 같은 이름이라 함께 막힌다.
        if (CppKeywords.Contains(lower) || MacroNames.Contains(lower) || lower is "std" or "rocks" or "posix")
        {
            throw new ArgumentException($"Project name '{name}' cannot be used: its lowercase form '{lower}' becomes a namespace and must not be a C++ " +
                "keyword, a standard or platform macro, std, rocks or posix.");
        }
    }

    /// <summary>템플릿 이름을 대소문자 무시로 담은 이름은 거부한다(치환 결과가 다시 템플릿 이름을 담는다, 11.6절).</summary>
    internal static void RequireDistinctFromTemplate(string name, string templateName)
    {
        if (name.Contains(templateName, StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException($"Project name '{name}' contains the template name {templateName}; choose a name without it.");
        }
    }

    // 세 철자: 그대로, 대문자, 소문자(11.3절).
    private static string[] Spellings(string name) => [name, name.ToUpperInvariant(), name.ToLowerInvariant()];

    // 두 단계 치환: 철자마다 자리 표시로 바꾼 뒤 자리 표시를 새 이름으로 바꾼다(앞 규칙의 결과를 뒤 규칙이 다시 바꾸지 않는다, UE와 같다).
    internal static string Replace(string text, string templateName, string name)
    {
        string[] from = Spellings(templateName);
        string[] to = Spellings(name);
        string[] marks = [.. from.Select((_, index) => $"\u0001{index}\u0001")];
        for (int index = 0; index < from.Length; ++index)
        {
            text = text.Replace(from[index], marks[index], StringComparison.Ordinal);
        }

        for (int index = 0; index < from.Length; ++index)
        {
            text = text.Replace(marks[index], to[index], StringComparison.Ordinal);
        }

        return text;
    }

    private static void Copy(ProjectTemplate template, string destination, string name)
    {
        Directory.CreateDirectory(destination);
        foreach ((string relative, string file) in Files(template))
        {
            string target = Path.Combine(destination, Replace(relative, template.TemplateName, name));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            if (Replaces(template, file))
            {
                (string text, bool bom) = ReadText(file);
                File.WriteAllText(target, Replace(text, template.TemplateName, name), new UTF8Encoding(bom));
            }
            else
            {
                File.Copy(file, target);
            }
        }
    }

    // 치환 대상 파일을 엄격한 UTF-8로 읽는다(잘못된 바이트는 오류, BOM 여부를 돌려준다, NUL 바이트는 텍스트가 아니라 거부).
    internal static (string Text, bool Bom) ReadText(string file)
    {
        byte[] bytes = File.ReadAllBytes(file);
        if (Array.IndexOf(bytes, (byte)0) >= 0)
        {
            throw new InvalidDataException($"{file} has a NUL byte; files in replaceInExtensions must be UTF-8 text.");
        }

        bool bom = bytes.AsSpan().StartsWith(Encoding.UTF8.Preamble);
        try
        {
            return (new UTF8Encoding(false, true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0)), bom);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException($"{file} is not valid UTF-8: {error.Message}", error);
        }
    }

    // 템플릿 파일(빌드·IDE 산출물과 ignore 제외, 링크 거부)의 (상대 경로 '/' 구분, 절대 경로)다.
    internal static IEnumerable<(string Relative, string File)> Files(ProjectTemplate template)
    {
        var pending = new Stack<string>([template.Directory]);
        var files = new List<(string Relative, string File)>();
        while (pending.Count != 0)
        {
            foreach (string entry in Directory.EnumerateFileSystemEntries(pending.Pop()))
            {
                if (FileLinks.IsLink(entry))
                {
                    throw new InvalidDataException($"Template {template.Directory} contains a link: {entry}");
                }

                string relative = Path.GetRelativePath(template.Directory, entry).Replace('\\', '/');
                // 템플릿 디렉터리의 빌드·IDE 산출물(out/·Saved/·점 디렉터리·최상위 *.slnx)은 기본으로 뺀다(GameProjectLayout과 같은 규칙).
                if (GameProjectLayout.IsExcluded(template.Directory, entry) || Ignored(template, relative))
                {
                    continue;
                }

                if (Directory.Exists(entry))
                {
                    pending.Push(entry);
                }
                else
                {
                    files.Add((relative, entry));
                }
            }
        }

        return files.OrderBy(file => file.Relative, StringComparer.Ordinal);
    }

    // ignore 항목: 파일이나 디렉터리(그 아래 전부), glob(*·?는 한 경로 성분 안).
    private static bool Ignored(ProjectTemplate template, string relative) => template.Ignore.Any(pattern =>
        new Regex("^" + Regex.Escape(pattern.TrimEnd('/')).Replace("\\*", "[^/]*", StringComparison.Ordinal).Replace("\\?", "[^/]", StringComparison.Ordinal) + "(/.*)?$",
            RegexOptions.CultureInvariant).IsMatch(relative));

    private static bool Replaces(ProjectTemplate template, string file) =>
        template.ReplaceInExtensions.Contains(Path.GetExtension(file), StringComparer.OrdinalIgnoreCase);

    // 결과에 남은 템플릿 이름(세 철자, 경로·치환 대상 내용)이다.
    private static IEnumerable<string> Leftovers(string destination, ProjectTemplate template)
    {
        string[] spellings = Spellings(template.TemplateName);
        foreach (string file in Directory.GetFiles(destination, "*", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(destination, file).Replace('\\', '/');
            if (spellings.Any(spelling => relative.Contains(spelling, StringComparison.Ordinal)))
            {
                yield return relative;
            }
            else if (Replaces(template, file) && spellings.Any(spelling => ReadText(file).Text.Contains(spelling, StringComparison.Ordinal)))
            {
                yield return relative + " (content)";
            }
        }
    }

    // engineVersion을 SDK 주·부로 맞추고 engineAssociation을 지운다(11.4절). 다른 필드와 순서는 그대로다.
    private static void WriteDescriptor(string path, EngineVersion engine)
    {
        JsonObject root = JsonNode.Parse(File.ReadAllText(path))!.AsObject();
        string version = $"{engine.Major}.{engine.Minor}";
        // 이미 맞으면 다시 쓰지 않는다(템플릿의 서식을 그대로 둔다).
        if (root["engineVersion"]?.GetValue<string>() == version && !root.ContainsKey("engineAssociation"))
        {
            return;
        }

        root["engineVersion"] = version;
        root.Remove("engineAssociation");
        AtomicFile.WriteIfChanged(path, root.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        }) + "\n");
        Rocks.Build.Descriptors.DescriptorReader.ReadProject(path);
    }

    // SDK MetaCompiler의 ids reissue로 명세를 새 ID로 다시 쓴다(명세 형식은 MetaCompiler만 다룬다).
    private static async Task ReissueAsync(string sdk, string manifest, CancellationToken cancellationToken)
    {
        string metaCompiler = Path.Combine(sdk, "Tools", "Rocks.MetaCompiler", "Rocks.MetaCompiler.dll");
        var result = await ProcessRunner.RunAsync(new("dotnet", [metaCompiler, "ids", "reissue", "--manifest", manifest], Path.GetDirectoryName(manifest)!),
            cancellationToken);
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException($"ids reissue failed for {manifest}: {result.StdOut}{result.StdErr}");
        }
    }

    // 이 CLI가 든 SDK다(<SDK>/Tools/Rocks.Build.Cli/). 엔진 작업 사본의 CLI면 --sdk가 필요하다.
    private static string OwnSdk()
    {
        string? root = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(typeof(NewProjectCommand).Assembly.Location)));
        return root is not null && File.Exists(Path.Combine(root, SdkAssociation.ManifestName)) ? root
            : throw new ArgumentException($"'{Name}' needs --sdk <installed SDK> when it does not run from an installed SDK.");
    }

    private static string Required(Dictionary<string, string> options, string key) =>
        options.TryGetValue(key, out string? value) && value.Length != 0 ? value : throw new ArgumentException($"'{Name}' requires --{key}.");
}
