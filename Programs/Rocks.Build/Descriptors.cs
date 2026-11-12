using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Build.Descriptors;

// 프로젝트·플러그인 기술자(.rocksproject·.rocksplugin)다. UE5.6의 .uproject·.uplugin과 같은 역할을 하며
// 필드 이름은 Rocks 규칙(camelCase)을 쓴다. 기술자는 UTF-8 JSON이며 다음을 엄격하게 검사한다.
// - 모르는 필드, 잘못된 타입, 중복 이름, 지원하지 않는 fileVersion은 오류다(조용히 무시하지 않는다).
// - 주석과 끝 쉼표는 받지 않는다. 1 MiB를 넘는 파일은 받지 않는다.
//
// 프로젝트 예(MyGame/MyGame.rocksproject):
//   {
//     "fileVersion": 1,
//     "engineVersion": "0.1",
//     "description": "예제 게임",
//     "modules": [ { "name": "MyGame", "type": "Runtime", "loadingPhase": "Default" } ],
//     "plugins": [ { "name": "Water", "enabled": true } ],
//     "targetPlatforms": ["Windows", "MacOS", "Android"]
//   }
//
// 플러그인 예(Engine/Plugins/Rendering/Water/Water.rocksplugin):
//   {
//     "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Water",
//     "category": "Rendering", "enabledByDefault": false,
//     "modules": [ { "name": "Water", "type": "Runtime", "platforms": ["Windows", "MacOS"] } ],
//     "plugins": [ { "name": "Physics", "enabled": true } ]
//   }

/// <summary>모듈 종류다. Runtime은 게임에 들어가고 Developer·Editor는 도구·에디터에서만 쓴다.</summary>
public enum ModuleType { Runtime, Developer, Editor }

/// <summary>모듈을 올리는 시점이다(UE ELoadingPhase의 일부). 기본은 Default다.</summary>
public enum LoadingPhase { EarliestPossible, PostConfigInit, PreDefault, Default, PostDefault }

/// <summary>
/// 기술자에 적힌 모듈 하나다.
/// Platforms가 null이면 모든 플랫폼, 아니면 나열한 플랫폼에서만 빌드·로드한다.
/// </summary>
public sealed record ModuleDescriptor(string Name, ModuleType Type, LoadingPhase LoadingPhase,
    IReadOnlyList<NativePlatform>? Platforms)
{
    /// <summary>platform에서 이 모듈을 쓰는지다.</summary>
    public bool SupportsPlatform(NativePlatform platform) => Platforms is null || Platforms.Contains(platform);
}

/// <summary>다른 플러그인 참조다. 프로젝트에서는 켜거나 끄고, 플러그인에서는 의존(enabled는 true)을 뜻한다.</summary>
public sealed record PluginReference(string Name, bool Enabled);

/// <summary>.rocksproject를 읽은 결과다. Name은 파일 이름(확장자 제외)이다.</summary>
public sealed record ProjectDescriptor(
    string Name,
    string FilePath,
    int FileVersion,
    string EngineVersion,
    string Description,
    IReadOnlyList<ModuleDescriptor> Modules,
    IReadOnlyList<PluginReference> Plugins,
    IReadOnlyList<NativePlatform> TargetPlatforms)
{
    /// <summary>프로젝트 폴더(기술자가 있는 폴더)다. 프로젝트 플러그인은 그 아래 Plugins/에 둔다.</summary>
    public string RootDirectory => Path.GetDirectoryName(FilePath)!;
}

/// <summary>.rocksplugin을 읽은 결과다. Name은 파일 이름이며 플러그인 폴더 이름과 같아야 한다.</summary>
public sealed record PluginDescriptor(
    string Name,
    string FilePath,
    int FileVersion,
    int Version,
    string VersionName,
    string FriendlyName,
    string Description,
    string Category,
    bool EnabledByDefault,
    IReadOnlyList<ModuleDescriptor> Modules,
    IReadOnlyList<PluginReference> Plugins)
{
    /// <summary>플러그인 폴더다. 모듈은 그 아래 Source/&lt;모듈&gt;/에 둔다.</summary>
    public string RootDirectory => Path.GetDirectoryName(FilePath)!;
}

/// <summary>기술자 형식·내용·해석 오류다. FilePath는 문제가 된 기술자(알 수 있을 때)다.</summary>
public sealed class DescriptorException(string? filePath, string message)
    : Exception(filePath is null ? message : $"{filePath}: {message}")
{
    public string? FilePath { get; } = filePath;
}

/// <summary>기술자를 엄격하게 읽고 검증한다.</summary>
public static class DescriptorReader
{
    /// <summary>지금 읽을 수 있는 기술자 형식 버전이다. 형식을 바꾸면 올리고 이전 버전 처리를 정한다.</summary>
    public const int CurrentFileVersion = 1;

    public const string ProjectExtension = ".rocksproject";
    public const string PluginExtension = ".rocksplugin";

    private const long MaximumBytes = 1024 * 1024;

    private static readonly Regex Identifier = new("^[A-Za-z][A-Za-z0-9_]{0,63}$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static readonly Regex VersionText = new("^[0-9]+\\.[0-9]+$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 프로젝트 기술자를 읽는다.
    /// 필수: fileVersion(=1), engineVersion("주.부", 예: "0.1"). 선택: description, modules, plugins,
    /// targetPlatforms(생략하면 모든 플랫폼). 오류는 DescriptorException이다.
    /// </summary>
    public static ProjectDescriptor ReadProject(string path)
    {
        string name = DescriptorName(path, ProjectExtension);
        using JsonDocument document = Parse(path);
        JsonElement root = document.RootElement;
        RequireKeys(path, root, "project", ["fileVersion", "engineVersion", "description", "modules", "plugins",
            "targetPlatforms"]);
        int fileVersion = ReadFileVersion(path, root);

        string engineVersion = RequiredString(path, root, "engineVersion");
        if (!VersionText.IsMatch(engineVersion))
            throw new DescriptorException(path, $"engineVersion '{engineVersion}' must look like '0.1'.");

        IReadOnlyList<NativePlatform> platforms = root.TryGetProperty("targetPlatforms", out JsonElement value)
            ? ReadPlatforms(path, value, "targetPlatforms")
            : Enum.GetValues<NativePlatform>();
        return new(name, Path.GetFullPath(path), fileVersion, engineVersion,
            OptionalString(path, root, "description") ?? "",
            ReadModules(path, root),
            ReadReferences(path, root, requireEnabled: false),
            platforms);
    }

    /// <summary>
    /// 플러그인 기술자를 읽는다. 파일 이름은 플러그인 폴더 이름과 같아야 한다(Water/Water.rocksplugin).
    /// 필수: fileVersion(=1), version(1 이상 정수), versionName, friendlyName. 선택: description, category,
    /// enabledByDefault(기본 false), modules, plugins(의존 플러그인, enabled는 true만). 오류는 DescriptorException이다.
    /// </summary>
    public static PluginDescriptor ReadPlugin(string path)
    {
        string name = DescriptorName(path, PluginExtension);
        string folder = Path.GetFileName(Path.GetDirectoryName(Path.GetFullPath(path))!);
        if (!string.Equals(folder, name, StringComparison.Ordinal))
            throw new DescriptorException(path, $"plugin descriptor name '{name}' must match its folder '{folder}'.");

        using JsonDocument document = Parse(path);
        JsonElement root = document.RootElement;
        RequireKeys(path, root, "plugin", ["fileVersion", "version", "versionName", "friendlyName", "description",
            "category", "enabledByDefault", "modules", "plugins"]);
        int fileVersion = ReadFileVersion(path, root);

        int version = RequiredInt(path, root, "version");
        if (version < 1)
            throw new DescriptorException(path, "version must be 1 or greater.");

        return new(name, Path.GetFullPath(path), fileVersion, version,
            RequiredString(path, root, "versionName"),
            RequiredString(path, root, "friendlyName"),
            OptionalString(path, root, "description") ?? "",
            OptionalString(path, root, "category") ?? "",
            OptionalBool(path, root, "enabledByDefault") ?? false,
            ReadModules(path, root),
            ReadReferences(path, root, requireEnabled: true));
    }

    // ---- 내부 ----

    private static string DescriptorName(string path, string extension)
    {
        if (!string.Equals(Path.GetExtension(path), extension, StringComparison.Ordinal))
            throw new DescriptorException(path, $"descriptor file must end with '{extension}'.");

        string name = Path.GetFileNameWithoutExtension(path);
        if (!Identifier.IsMatch(name))
            throw new DescriptorException(path, $"descriptor name '{name}' must be an identifier (letters, digits, '_').");

        return name;
    }

    private static JsonDocument Parse(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists)
            throw new DescriptorException(path, "descriptor file does not exist.");

        if (info.Length > MaximumBytes)
            throw new DescriptorException(path, "descriptor file is larger than 1 MiB.");

        string text;
        try
        {
            text = File.ReadAllText(path, new UTF8Encoding(false, true));
        }
        catch (DecoderFallbackException)
        {
            throw new DescriptorException(path, "descriptor file is not valid UTF-8.");
        }

        try
        {
            JsonDocument document = JsonDocument.Parse(text, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 16
            });
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                document.Dispose();
                throw new DescriptorException(path, "descriptor root must be a JSON object.");
            }

            return document;
        }
        catch (JsonException error)
        {
            throw new DescriptorException(path, "invalid JSON: " + error.Message);
        }
    }

    private static void RequireKeys(string path, JsonElement element, string what, IReadOnlyList<string> allowed)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (!allowed.Contains(property.Name))
                throw new DescriptorException(path, $"unknown {what} field '{property.Name}'.");

            if (!seen.Add(property.Name))
                throw new DescriptorException(path, $"duplicate {what} field '{property.Name}'.");
        }
    }

    private static int ReadFileVersion(string path, JsonElement root)
    {
        int version = RequiredInt(path, root, "fileVersion");
        if (version != CurrentFileVersion)
            throw new DescriptorException(path,
                $"fileVersion {version} is not supported (supported: {CurrentFileVersion}).");

        return version;
    }

    private static IReadOnlyList<ModuleDescriptor> ReadModules(string path, JsonElement root)
    {
        if (!root.TryGetProperty("modules", out JsonElement modules))
            return [];

        RequireKind(path, modules, JsonValueKind.Array, "modules");
        var result = new List<ModuleDescriptor>();
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement module in modules.EnumerateArray())
        {
            RequireKind(path, module, JsonValueKind.Object, "modules[]");
            RequireKeys(path, module, "module", ["name", "type", "loadingPhase", "platforms"]);
            string name = RequiredIdentifier(path, module, "name");
            // 대소문자만 다른 모듈은 대소문자를 구분하지 않는 파일 시스템에서 같은 폴더가 된다.
            if (!names.Add(name))
                throw new DescriptorException(path, $"duplicate module '{name}'.");

            ModuleType type = RequiredEnum<ModuleType>(path, module, "type");
            LoadingPhase phase = module.TryGetProperty("loadingPhase", out _)
                ? RequiredEnum<LoadingPhase>(path, module, "loadingPhase")
                : LoadingPhase.Default;
            IReadOnlyList<NativePlatform>? platforms = module.TryGetProperty("platforms", out JsonElement value)
                ? ReadPlatforms(path, value, $"modules '{name}' platforms")
                : null;
            result.Add(new(name, type, phase, platforms));
        }

        return result;
    }

    private static IReadOnlyList<PluginReference> ReadReferences(string path, JsonElement root, bool requireEnabled)
    {
        if (!root.TryGetProperty("plugins", out JsonElement plugins))
            return [];

        RequireKind(path, plugins, JsonValueKind.Array, "plugins");
        var result = new List<PluginReference>();
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement reference in plugins.EnumerateArray())
        {
            RequireKind(path, reference, JsonValueKind.Object, "plugins[]");
            RequireKeys(path, reference, "plugin reference", ["name", "enabled"]);
            string name = RequiredIdentifier(path, reference, "name");
            if (!names.Add(name))
                throw new DescriptorException(path, $"duplicate plugin reference '{name}'.");

            bool enabled = OptionalBool(path, reference, "enabled")
                ?? throw new DescriptorException(path, $"plugin reference '{name}' requires 'enabled'.");
            // 플러그인의 plugins는 의존 목록이다. 의존을 끄는 표기는 뜻이 없으므로 받지 않는다.
            if (requireEnabled && !enabled)
                throw new DescriptorException(path, $"plugin dependency '{name}' must have \"enabled\": true.");

            result.Add(new(name, enabled));
        }

        return result;
    }

    private static IReadOnlyList<NativePlatform> ReadPlatforms(string path, JsonElement value, string what)
    {
        RequireKind(path, value, JsonValueKind.Array, what);
        var result = new List<NativePlatform>();
        foreach (JsonElement item in value.EnumerateArray())
        {
            RequireKind(path, item, JsonValueKind.String, what + "[]");
            string text = item.GetString()!;
            if (!Enum.TryParse(text, ignoreCase: false, out NativePlatform platform) ||
                !Enum.IsDefined(platform) || !string.Equals(platform.ToString(), text, StringComparison.Ordinal))
                throw new DescriptorException(path,
                    $"unknown platform '{text}' in {what} (expected: {string.Join(", ", Enum.GetNames<NativePlatform>())}).");

            if (result.Contains(platform))
                throw new DescriptorException(path, $"duplicate platform '{text}' in {what}.");

            result.Add(platform);
        }

        if (result.Count == 0)
            throw new DescriptorException(path, $"{what} must not be empty (omit it to allow every platform).");

        return result;
    }

    private static void RequireKind(string path, JsonElement element, JsonValueKind kind, string what)
    {
        if (element.ValueKind != kind)
            throw new DescriptorException(path, $"{what} must be a JSON {kind.ToString().ToLowerInvariant()}.");
    }

    private static string RequiredString(string path, JsonElement element, string key)
    {
        string? value = OptionalString(path, element, key);
        if (string.IsNullOrWhiteSpace(value))
            throw new DescriptorException(path, $"'{key}' is required and must be a non-empty string.");

        return value;
    }

    private static string? OptionalString(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
            return null;

        RequireKind(path, value, JsonValueKind.String, key);
        return value.GetString();
    }

    private static string RequiredIdentifier(string path, JsonElement element, string key)
    {
        string value = RequiredString(path, element, key);
        if (!Identifier.IsMatch(value))
            throw new DescriptorException(path, $"'{value}' is not a valid name (letters, digits, '_', max 64).");

        return value;
    }

    private static int RequiredInt(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
            throw new DescriptorException(path, $"'{key}' is required.");

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out int result))
            throw new DescriptorException(path, $"'{key}' must be an integer.");

        return result;
    }

    private static bool? OptionalBool(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
            return null;

        if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            throw new DescriptorException(path, $"'{key}' must be true or false.");

        return value.GetBoolean();
    }

    private static T RequiredEnum<T>(string path, JsonElement element, string key) where T : struct, Enum
    {
        string text = RequiredString(path, element, key);
        if (!Enum.TryParse(text, ignoreCase: false, out T value) || !Enum.IsDefined(value) ||
            !string.Equals(value.ToString(), text, StringComparison.Ordinal))
            throw new DescriptorException(path,
                $"'{key}' value '{text}' is not one of: {string.Join(", ", Enum.GetNames<T>())}.");

        return value;
    }
}

/// <summary>플러그인을 찾고 프로젝트에 맞게 켤 플러그인을 정한다.</summary>
public static class PluginCatalog
{
    /// <summary>
    /// roots 아래에서 &lt;이름&gt;/&lt;이름&gt;.rocksplugin을 모두 찾아 읽는다. 엔진 플러그인 폴더(Engine/Plugins)와
    /// 프로젝트 플러그인 폴더(&lt;프로젝트&gt;/Plugins)를 넘긴다. 없는 root는 건너뛴다.
    /// - 플러그인 폴더 안에 다른 플러그인을 두지 않는다(찾은 플러그인 폴더 아래는 더 찾지 않는다).
    /// - 같은 이름의 플러그인이 둘 이상이면(대소문자 무시) 오류다.
    /// 결과는 이름 순이다.
    /// </summary>
    public static IReadOnlyList<PluginDescriptor> Discover(IEnumerable<string> roots)
    {
        var found = new Dictionary<string, PluginDescriptor>(StringComparer.OrdinalIgnoreCase);
        foreach (string root in roots)
        {
            if (Directory.Exists(root))
                Visit(Path.GetFullPath(root), found);
        }

        return found.Values.OrderBy(plugin => plugin.Name, StringComparer.Ordinal).ToList();
    }

    /// <summary>
    /// project가 쓸 플러그인을 의존 순서(의존하는 플러그인이 뒤)로 정한다.
    /// 1) 프로젝트가 enabled: true로 적은 플러그인과, 적지 않은 플러그인 중 enabledByDefault인 것을 켠다.
    ///    프로젝트가 enabled: false로 적은 플러그인은 기본값과 관계없이 끈다.
    /// 2) 켠 플러그인의 의존 플러그인을 모두 켠다.
    /// 오류(DescriptorException): 프로젝트·플러그인이 없는 플러그인을 참조, 켠 플러그인이 프로젝트가 끈
    /// 플러그인에 의존, 의존 순환.
    /// </summary>
    public static IReadOnlyList<PluginDescriptor> ResolveEnabled(ProjectDescriptor project,
        IReadOnlyList<PluginDescriptor> available)
    {
        var byName = new Dictionary<string, PluginDescriptor>(StringComparer.Ordinal);
        foreach (PluginDescriptor plugin in available)
        {
            if (!byName.TryAdd(plugin.Name, plugin))
                throw new DescriptorException(plugin.FilePath, $"duplicate plugin '{plugin.Name}'.");
        }

        var explicitChoice = new Dictionary<string, bool>(StringComparer.Ordinal);
        foreach (PluginReference reference in project.Plugins)
        {
            if (!byName.ContainsKey(reference.Name))
                throw new DescriptorException(project.FilePath, $"project references unknown plugin '{reference.Name}'.");

            explicitChoice[reference.Name] = reference.Enabled;
        }

        // 1) 처음 켤 플러그인.
        var enabled = new HashSet<string>(StringComparer.Ordinal);
        foreach (PluginDescriptor plugin in byName.Values)
        {
            bool on = explicitChoice.TryGetValue(plugin.Name, out bool choice) ? choice : plugin.EnabledByDefault;
            if (on)
                enabled.Add(plugin.Name);
        }

        // 2) 의존을 따라 켜고, 깊이 우선으로 순서를 정하며 순환을 찾는다.
        var ordered = new List<PluginDescriptor>();
        var state = new Dictionary<string, bool>(StringComparer.Ordinal); // false=방문 중, true=완료
        foreach (string name in enabled.OrderBy(name => name, StringComparer.Ordinal).ToList())
            Visit(name, requiredBy: null);

        return ordered;

        void Visit(string name, PluginDescriptor? requiredBy)
        {
            if (!byName.TryGetValue(name, out PluginDescriptor? plugin))
                throw new DescriptorException(requiredBy?.FilePath, $"plugin '{requiredBy?.Name}' depends on unknown plugin '{name}'.");

            if (requiredBy is not null && explicitChoice.TryGetValue(name, out bool choice) && !choice)
                throw new DescriptorException(project.FilePath,
                    $"plugin '{requiredBy.Name}' requires '{name}', but the project disables it.");

            if (state.TryGetValue(name, out bool done))
            {
                if (!done)
                    throw new DescriptorException(plugin.FilePath, $"plugin dependency cycle reaches '{name}'.");

                return;
            }

            state[name] = false;
            foreach (PluginReference dependency in plugin.Plugins.OrderBy(reference => reference.Name, StringComparer.Ordinal))
                Visit(dependency.Name, plugin);

            state[name] = true;
            ordered.Add(plugin);
        }
    }

    private static void Visit(string directory, Dictionary<string, PluginDescriptor> found)
    {
        string name = Path.GetFileName(directory);
        string candidate = Path.Combine(directory, name + DescriptorReader.PluginExtension);
        if (File.Exists(candidate))
        {
            PluginDescriptor plugin = DescriptorReader.ReadPlugin(candidate);
            if (found.TryGetValue(plugin.Name, out PluginDescriptor? existing))
                throw new DescriptorException(plugin.FilePath,
                    $"duplicate plugin '{plugin.Name}' (also at {existing.FilePath}).");

            found.Add(plugin.Name, plugin);
            return;
        }

        foreach (string child in Directory.EnumerateDirectories(directory).Order(StringComparer.Ordinal))
        {
            string childName = Path.GetFileName(child);
            if (childName is "bin" or "obj" or "out" or ".git")
                continue;

            if ((File.GetAttributes(child) & FileAttributes.ReparsePoint) != 0)
                continue; // link를 따라가 같은 플러그인을 두 번 찾거나 저장소 밖을 읽지 않는다

            Visit(child, found);
        }
    }
}
