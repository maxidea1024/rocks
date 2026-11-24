using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Build.Rules;

namespace Rocks.Build.Descriptors;

// 프로젝트·플러그인 기술자(.rocksproject·.rocksplugin)다. UE5.8의 .uproject·.uplugin과 같은 역할을 하며
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

/// <summary>
/// 모듈 종류다(Docs/Design/BuildTargets 3.3절 BD4). 들어가는 타깃은 <see cref="ModuleTypeRules.Allows"/>다: Runtime은 모든 타깃, Developer는
/// Release 출시 타깃(Game·Client·Server)을 뺀 곳, Editor는 Editor 타깃과 CompileWithEditor Program, ServerOnly는 Client를 뺀 곳, ClientOnly는
/// Server를 뺀 곳이다.
/// </summary>
public enum ModuleType { Runtime, Developer, Editor, ServerOnly, ClientOnly }

/// <summary>
/// 모듈을 올리는 시점이다(UE ELoadingPhase의 일부, Docs/Design/ModuleLoader 3.3절). 기본은 Default다. 값 순서가 단계 순서이고 Core
/// <c>rocks::LoadingPhase</c>와 같다. None은 자동으로 올리지 않는다(코드가 Load로 올린다).
/// </summary>
public enum LoadingPhase { EarliestPossible, PostConfigInit, PreDefault, Default, PostDefault, PostEngineInit, None }

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
/// <summary>
/// 플러그인 참조의 구성 조건 값이다(Docs/Design/BuildTargets 3.6절, UE TargetConfigurationAllowList). Asan·Coverage 구성은 Debug로 본다.
/// </summary>
public enum PluginConfiguration { Debug, Development, Release }

/// <summary>
/// 플러그인 해석 문맥이다(Docs/Design/BuildTargets 3.6절 BS4): 이번 빌드의 타깃 종류·플랫폼·구성. 조건 있는 참조와 플러그인 지원 플랫폼을
/// 이것으로 판정한다.
/// </summary>
public sealed record PluginContext(TargetType Target, NativePlatform Platform, NativeConfiguration Configuration)
{
    /// <summary>참조 조건의 구성 값이다(Asan·Coverage → Debug).</summary>
    public PluginConfiguration PluginConfiguration => Configuration switch
    {
        NativeConfiguration.Development => PluginConfiguration.Development,
        NativeConfiguration.Release => PluginConfiguration.Release,
        _ => PluginConfiguration.Debug
    };

    /// <summary>오류 문구용 철자다.</summary>
    public override string ToString() => $"{Target} {Platform} {PluginConfiguration}";
}

/// <summary>
/// 플러그인 참조다. 프로젝트 기술자의 <c>plugins[]</c>와 타깃 규칙의 <c>Plugins</c>는 조건 칸(<see cref="Targets"/>·<see cref="Platforms"/>·
/// <see cref="Configurations"/> — null은 조건 없음, Docs/Design/BuildTargets 3.6절 BD12)을 쓸 수 있고, 플러그인 기술자의 의존 참조는 쓸 수
/// 없다(BD23). 조건은 <c>Enabled</c>가 참일 때만 둔다. 문맥이 조건에 맞지 않으면 <c>enabled: false</c>로 적은 것과 같다(UE 의미론). 사용 예:
/// <code>
/// new PluginReference("ModelingTools", true) { Targets = [TargetType.Editor] }
/// </code>
/// </summary>
public sealed record PluginReference(string Name, bool Enabled)
{
    /// <summary>켜는 타깃 종류다(null = 모두).</summary>
    public IReadOnlyList<TargetType>? Targets { get; init; }

    /// <summary>켜는 플랫폼이다(null = 모두).</summary>
    public IReadOnlyList<NativePlatform>? Platforms { get; init; }

    /// <summary>켜는 구성이다(null = 모두).</summary>
    public IReadOnlyList<PluginConfiguration>? Configurations { get; init; }

    /// <summary>조건 칸이 하나라도 있는가다.</summary>
    public bool HasConditions => Targets is not null || Platforms is not null || Configurations is not null;

    /// <summary>문맥이 조건에 맞는가다(조건 없는 참조는 늘 맞다).</summary>
    public bool Matches(PluginContext context) =>
        (Targets is null || Targets.Contains(context.Target)) && (Platforms is null || Platforms.Contains(context.Platform)) &&
        (Configurations is null || Configurations.Contains(context.PluginConfiguration));

    /// <summary>오류 문구용 조건 철자다(예: <c>targets [Editor], platforms [Windows]</c>).</summary>
    public string ConditionText =>
        string.Join(", ", new[]
        {
            Targets is null ? null : "targets [" + string.Join(", ", Targets) + "]",
            Platforms is null ? null : "platforms [" + string.Join(", ", Platforms) + "]",
            Configurations is null ? null : "configurations [" + string.Join(", ", Configurations) + "]"
        }.OfType<string>());

    /// <summary>
    /// 조건 칸의 값 규칙이다(문맥 무관): 조건은 <c>Enabled</c>일 때만, 빈 목록·중복·정의되지 않은 값 없음. 어기면 문구를 돌려준다(맞으면 null).
    /// </summary>
    public string? ConditionProblem()
    {
        if (HasConditions && !Enabled)
        {
            return $"plugin reference '{Name}' has conditions but \"enabled\": false; conditions apply only to enabled references";
        }

        static string? Problem<T>(string key, IReadOnlyList<T>? values) where T : struct, Enum =>
            values is null ? null
            : values.Count == 0 ? $"'{key}' must not be empty"
            : values.Any(value => !Enum.IsDefined(value)) ? $"'{key}' has an unknown value"
            : values.Distinct().Count() != values.Count ? $"'{key}' lists a value twice"
            : null;

        string? problem = Problem("targets", Targets) ?? Problem("platforms", Platforms) ?? Problem("configurations", Configurations);
        return problem is null ? null : $"plugin reference '{Name}': {problem}";
    }
}

/// <summary>
/// 플러그인 참조를 적은 쪽이다(Docs/Design/BuildTargets 3.9절). Label은 오류 문구의 주어("project", "target 'Commandlet'")이고,
/// FilePath는 오류가 가리킬 기술자(타깃 규칙만이면 null)다.
/// </summary>
public sealed record PluginReferrer(string Label, string? FilePath)
{
    /// <summary>프로젝트 기술자다.</summary>
    public static PluginReferrer Of(ProjectDescriptor project) => new("project", project.FilePath);

    /// <summary>타깃 규칙이다(엔진 저장소 타깃은 기술자 파일이 없다).</summary>
    public static PluginReferrer OfTarget(string targetName) => new($"target '{targetName}'", null);
}

/// <summary>
/// 타깃 규칙의 플러그인 참조(TargetRules.Plugins)를 다룬다(Docs/Design/BuildTargets 3.9절 BD33).
/// </summary>
public static class TargetPluginReferences
{
    private static readonly Regex Identifier = new("^[A-Za-z][A-Za-z0-9_]{0,63}$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 타깃 규칙의 참조 목록을 검사한다. 이름은 기술자와 같은 식별자여야 하고, 같은 이름(대소문자 무시)을 두 번 적으면 오류다.
    /// 플러그인이 있는지는 해석(<see cref="PluginCatalog.ResolveEnabled"/>)이 본다.
    /// </summary>
    /// <exception cref="DescriptorException">이름 형식·중복.</exception>
    public static void Validate(string targetName, IReadOnlyList<PluginReference> references)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (PluginReference reference in references)
        {
            if (!Identifier.IsMatch(reference.Name))
            {
                throw new DescriptorException(null,
                    $"target '{targetName}' plugin reference '{reference.Name}' is not a valid name (letters, digits, '_', max 64).");
            }

            if (!names.Add(reference.Name))
            {
                throw new DescriptorException(null, $"target '{targetName}' references plugin '{reference.Name}' more than once.");
            }

            if (reference.ConditionProblem() is { } problem)
            {
                throw new DescriptorException(null, $"target '{targetName}' {problem}.");
            }
        }
    }

    /// <summary>
    /// 타깃 참조와 프로젝트 참조를 합친다. 같은 이름은 타깃 참조가 이긴다(UE SetupPlugins가 타깃 목록을 먼저 처리하고 프로젝트 참조는
    /// 이미 적힌 이름을 건너뛰는 것과 같다). 순서는 타깃 참조, 그 뒤 남은 프로젝트 참조다.
    /// </summary>
    public static IReadOnlyList<PluginReference> Combine(IReadOnlyList<PluginReference> target, IReadOnlyList<PluginReference> project)
    {
        var names = target.Select(reference => reference.Name).ToHashSet(StringComparer.Ordinal);
        return [.. target, .. project.Where(reference => !names.Contains(reference.Name))];
    }
}

/// <summary>
/// 프로젝트가 대상으로 하는 엔진의 주·부 버전이다(engineVersion "주.부", 각 0–65535). 설치 SDK 게임 빌드가 SDK manifest의 엔진
/// 버전과 주·부가 같은지 본다(<see cref="EngineVersion.CanLinkProject"/>, Docs/Design/ReflectionFoundations.ko.md 8.2절).
/// </summary>
public readonly record struct ProjectEngineVersion(ushort Major, ushort Minor)
{
    /// <summary>기술자 표기 "주.부"다(예: "0.1").</summary>
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"{Major}.{Minor}");
}

/// <summary>.rocksproject를 읽은 결과다. Name은 파일 이름(확장자 제외)이다.</summary>
public sealed record ProjectDescriptor(
    string Name,
    string FilePath,
    int FileVersion,
    ProjectEngineVersion EngineVersion,
    string Description,
    IReadOnlyList<ModuleDescriptor> Modules,
    IReadOnlyList<PluginReference> Plugins,
    IReadOnlyList<NativePlatform> TargetPlatforms)
{
    /// <summary>프로젝트 폴더(기술자가 있는 폴더)다. 프로젝트 플러그인은 그 아래 Plugins/에 둔다.</summary>
    public string RootDirectory => Path.GetDirectoryName(FilePath)!;

    /// <summary>
    /// 설치 SDK 연결(선택 필드 engineAssociation — SDK 경로 또는 등록 ID <c>{GUID}</c>)이다. 없으면 null이고, 게임 도구가 engineVersion으로
    /// 등록 SDK를 고른다(Docs/Design/ProjectFiles.ko.md 10.1절).
    /// </summary>
    public string? EngineAssociation { get; init; }
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

    /// <summary>
    /// 지원 플랫폼이다(선택 필드 platforms — null은 모든 플랫폼, Docs/Design/BuildTargets 3.6절 BD13). 지원하지 않는 플랫폼에서 기본 활성으로만
    /// 켜질 플러그인은 조용히 빠지고, 명시·의존으로 켜면 오류다.
    /// </summary>
    public IReadOnlyList<NativePlatform>? Platforms { get; init; }
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

    // 정규 10진 "주.부"다(앞자리 0 없음, ASCII 숫자만). 각 값의 범위(0–65535)는 ReadProject가 따로 본다. 끝은 \z다(.NET의 $는 마지막
    // 줄바꿈 앞에서도 맞아 "0.1\n"을 받는다).
    private static readonly Regex VersionText = new("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 프로젝트 기술자를 읽는다.
    /// 필수: fileVersion(=1), engineVersion("주.부", 예: "0.1" — 앞자리 0 없는 10진, 각 0–65535). 선택: engineAssociation(설치 SDK 경로 또는 등록 ID), description, modules,
    /// plugins, targetPlatforms(생략하면 모든 플랫폼). 오류는 DescriptorException이다. engineVersion이 엔진과 맞는지는 게임 빌드가
    /// 설치 SDK의 엔진 버전과 대조한다(여기서는 형식만 본다).
    /// </summary>
    public static ProjectDescriptor ReadProject(string path)
    {
        string name = DescriptorName(path, ProjectExtension);
        using JsonDocument document = Parse(path);
        JsonElement root = document.RootElement;
        RequireKeys(path, root, "project", ["fileVersion", "engineVersion", "engineAssociation", "description", "modules", "plugins",
            "targetPlatforms"]);
        int fileVersion = ReadFileVersion(path, root);

        string engineText = RequiredString(path, root, "engineVersion");
        Match version = VersionText.Match(engineText);
        if (!version.Success ||
            !ushort.TryParse(version.Groups[1].ValueSpan, NumberStyles.None, CultureInfo.InvariantCulture, out ushort major) ||
            !ushort.TryParse(version.Groups[2].ValueSpan, NumberStyles.None, CultureInfo.InvariantCulture, out ushort minor))
        {
            throw new DescriptorException(path,
                $"engineVersion '{engineText}' must be '<major>.<minor>' in decimal without leading zeros, each 0-{ushort.MaxValue} (e.g. '0.1').");
        }

        var engineVersion = new ProjectEngineVersion(major, minor);
        IReadOnlyList<NativePlatform> platforms = root.TryGetProperty("targetPlatforms", out JsonElement value)
            ? ReadEnumValues<NativePlatform>(path, value, "targetPlatforms")
            : Enum.GetValues<NativePlatform>();
        IReadOnlyList<PluginReference> references = ReadReferences(path, root, requireEnabled: false);
        // 참조의 플랫폼 조건은 프로젝트가 대상으로 하는 플랫폼 안이어야 한다(Docs/Design/BuildTargets 3.6절).
        RequireTargetPlatforms(path, "project", references, platforms);

        string? association = OptionalString(path, root, "engineAssociation");
        if (association is not null && association.Trim().Length == 0)
        {
            throw new DescriptorException(path, "engineAssociation must be an SDK path or a registered SDK id; remove the field to pick an SDK by engineVersion.");
        }

        return new(name, Path.GetFullPath(path), fileVersion, engineVersion,
            OptionalString(path, root, "description") ?? "",
            ReadModules(path, root),
            references,
            platforms)
        {
            EngineAssociation = association
        };
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
        {
            throw new DescriptorException(path, $"plugin descriptor name '{name}' must match its folder '{folder}'.");
        }

        using JsonDocument document = Parse(path);
        JsonElement root = document.RootElement;
        RequireKeys(path, root, "plugin", ["fileVersion", "version", "versionName", "friendlyName", "description",
            "category", "enabledByDefault", "modules", "plugins", "platforms"]);
        int fileVersion = ReadFileVersion(path, root);

        int version = RequiredInt(path, root, "version");
        if (version < 1)
        {
            throw new DescriptorException(path, "version must be 1 or greater.");
        }

        return new(name, Path.GetFullPath(path), fileVersion, version,
            RequiredString(path, root, "versionName"),
            RequiredString(path, root, "friendlyName"),
            OptionalString(path, root, "description") ?? "",
            OptionalString(path, root, "category") ?? "",
            OptionalBool(path, root, "enabledByDefault") ?? false,
            ReadModules(path, root),
            ReadReferences(path, root, requireEnabled: true))
        {
            // 지원 플랫폼(BD13 — 생략은 모든 플랫폼).
            Platforms = ReadEnumList<NativePlatform>(path, root, "platforms", "plugin")
        };
    }

    // ---- 내부 ----

    private static string DescriptorName(string path, string extension)
    {
        if (!string.Equals(Path.GetExtension(path), extension, StringComparison.Ordinal))
        {
            throw new DescriptorException(path, $"descriptor file must end with '{extension}'.");
        }

        string name = Path.GetFileNameWithoutExtension(path);
        if (!Identifier.IsMatch(name))
        {
            throw new DescriptorException(path, $"descriptor name '{name}' must be an identifier (letters, digits, '_').");
        }

        return name;
    }

    private static JsonDocument Parse(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists)
        {
            throw new DescriptorException(path, "descriptor file does not exist.");
        }

        if (info.Length > MaximumBytes)
        {
            throw new DescriptorException(path, "descriptor file is larger than 1 MiB.");
        }

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
            {
                throw new DescriptorException(path, $"unknown {what} field '{property.Name}'.");
            }

            if (!seen.Add(property.Name))
            {
                throw new DescriptorException(path, $"duplicate {what} field '{property.Name}'.");
            }
        }
    }

    private static int ReadFileVersion(string path, JsonElement root)
    {
        int version = RequiredInt(path, root, "fileVersion");
        if (version != CurrentFileVersion)
        {
            throw new DescriptorException(path,
                $"fileVersion {version} is not supported (supported: {CurrentFileVersion}).");
        }

        return version;
    }

    private static IReadOnlyList<ModuleDescriptor> ReadModules(string path, JsonElement root)
    {
        if (!root.TryGetProperty("modules", out JsonElement modules))
        {
            return [];
        }

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
            {
                throw new DescriptorException(path, $"duplicate module '{name}'.");
            }

            ModuleType type = RequiredEnum<ModuleType>(path, module, "type");
            LoadingPhase phase = module.TryGetProperty("loadingPhase", out _)
                ? RequiredEnum<LoadingPhase>(path, module, "loadingPhase")
                : LoadingPhase.Default;
            IReadOnlyList<NativePlatform>? platforms = module.TryGetProperty("platforms", out JsonElement value)
                ? ReadEnumValues<NativePlatform>(path, value, $"modules '{name}' platforms")
                : null;
            result.Add(new(name, type, phase, platforms));
        }

        return result;
    }

    private static IReadOnlyList<PluginReference> ReadReferences(string path, JsonElement root, bool requireEnabled)
    {
        if (!root.TryGetProperty("plugins", out JsonElement plugins))
        {
            return [];
        }

        RequireKind(path, plugins, JsonValueKind.Array, "plugins");
        var result = new List<PluginReference>();
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement reference in plugins.EnumerateArray())
        {
            RequireKind(path, reference, JsonValueKind.Object, "plugins[]");
            // 플러그인의 의존 참조는 조건을 받지 않는다(Docs/Design/BuildTargets 3.6절 BD23).
            RequireKeys(path, reference, "plugin reference", requireEnabled ? ["name", "enabled"]
                : ["name", "enabled", "targets", "platforms", "configurations"]);
            string name = RequiredIdentifier(path, reference, "name");
            if (!names.Add(name))
            {
                throw new DescriptorException(path, $"duplicate plugin reference '{name}'.");
            }

            bool enabled = OptionalBool(path, reference, "enabled")
                ?? throw new DescriptorException(path, $"plugin reference '{name}' requires 'enabled'.");
            // 플러그인의 plugins는 의존 목록이다. 의존을 끄는 표기는 뜻이 없으므로 받지 않는다.
            if (requireEnabled && !enabled)
            {
                throw new DescriptorException(path, $"plugin dependency '{name}' must have \"enabled\": true.");
            }

            var read = new PluginReference(name, enabled)
            {
                Targets = ReadEnumList<TargetType>(path, reference, "targets", $"plugin reference '{name}'"),
                Platforms = ReadEnumList<NativePlatform>(path, reference, "platforms", $"plugin reference '{name}'"),
                Configurations = ReadEnumList<PluginConfiguration>(path, reference, "configurations", $"plugin reference '{name}'")
            };
            if (read.ConditionProblem() is { } problem)
            {
                throw new DescriptorException(path, problem + ".");
            }

            result.Add(read);
        }

        return result;
    }

    // 선택 enum 이름 목록이다(owner의 key — 없으면 null).
    private static IReadOnlyList<T>? ReadEnumList<T>(string path, JsonElement owner, string key, string what) where T : struct, Enum =>
        owner.TryGetProperty(key, out JsonElement value) ? ReadEnumValues<T>(path, value, $"{what} '{key}'") : null;

    // enum 이름 목록이다(플랫폼·타깃 종류·구성 — 기술자 읽기가 모두 이것을 쓴다). 빈 목록·중복·모르는 이름(정확한 철자 — 대소문자·숫자 철자는
    // 받지 않는다)은 오류다.
    private static IReadOnlyList<T> ReadEnumValues<T>(string path, JsonElement value, string what) where T : struct, Enum
    {
        RequireKind(path, value, JsonValueKind.Array, what);
        var result = new List<T>();
        foreach (JsonElement item in value.EnumerateArray())
        {
            RequireKind(path, item, JsonValueKind.String, what + "[]");
            string text = item.GetString()!;
            if (!Enum.TryParse(text, ignoreCase: false, out T parsed) || !Enum.IsDefined(parsed) ||
                !string.Equals(parsed.ToString(), text, StringComparison.Ordinal))
            {
                throw new DescriptorException(path, $"unknown value '{text}' in {what} (expected: {string.Join(", ", Enum.GetNames<T>())}).");
            }

            if (result.Contains(parsed))
            {
                throw new DescriptorException(path, $"{what} lists '{text}' twice.");
            }

            result.Add(parsed);
        }

        if (result.Count == 0)
        {
            throw new DescriptorException(path, $"{what} must not be empty (leave the field out to allow every value).");
        }

        return result;
    }

    /// <summary>
    /// 참조의 플랫폼 조건이 프로젝트 targetPlatforms 안인지 검사한다(Docs/Design/BuildTargets 3.6절 — 프로젝트 기술자와 게임 타깃 규칙 참조가
    /// 같이 쓴다). 밖이면 DescriptorException이다.
    /// </summary>
    internal static void RequireTargetPlatforms(string? path, string label, IReadOnlyList<PluginReference> references,
        IReadOnlyList<NativePlatform> targetPlatforms)
    {
        if (references.FirstOrDefault(reference => reference.Platforms?.Any(platform => !targetPlatforms.Contains(platform)) == true) is { } outside)
        {
            throw new DescriptorException(path, $"{label} plugin reference '{outside.Name}' lists a platform outside targetPlatforms " +
                $"({string.Join(", ", targetPlatforms)}).");
        }
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
            {
                throw new DescriptorException(path,
                    $"unknown platform '{text}' in {what} (expected: {string.Join(", ", Enum.GetNames<NativePlatform>())}).");
            }

            if (result.Contains(platform))
            {
                throw new DescriptorException(path, $"duplicate platform '{text}' in {what}.");
            }

            result.Add(platform);
        }

        if (result.Count == 0)
        {
            throw new DescriptorException(path, $"{what} must not be empty (omit it to allow every platform).");
        }

        return result;
    }

    private static void RequireKind(string path, JsonElement element, JsonValueKind kind, string what)
    {
        if (element.ValueKind != kind)
        {
            throw new DescriptorException(path, $"{what} must be a JSON {kind.ToString().ToLowerInvariant()}.");
        }
    }

    private static string RequiredString(string path, JsonElement element, string key)
    {
        string? value = OptionalString(path, element, key);
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new DescriptorException(path, $"'{key}' is required and must be a non-empty string.");
        }

        return value;
    }

    private static string? OptionalString(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
        {
            return null;
        }

        RequireKind(path, value, JsonValueKind.String, key);
        return value.GetString();
    }

    private static string RequiredIdentifier(string path, JsonElement element, string key)
    {
        string value = RequiredString(path, element, key);
        if (!Identifier.IsMatch(value))
        {
            throw new DescriptorException(path, $"'{value}' is not a valid name (letters, digits, '_', max 64).");
        }

        return value;
    }

    private static int RequiredInt(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
        {
            throw new DescriptorException(path, $"'{key}' is required.");
        }

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out int result))
        {
            throw new DescriptorException(path, $"'{key}' must be an integer.");
        }

        return result;
    }

    private static bool? OptionalBool(string path, JsonElement element, string key)
    {
        if (!element.TryGetProperty(key, out JsonElement value))
        {
            return null;
        }

        if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            throw new DescriptorException(path, $"'{key}' must be true or false.");
        }

        return value.GetBoolean();
    }

    private static T RequiredEnum<T>(string path, JsonElement element, string key) where T : struct, Enum
    {
        string text = RequiredString(path, element, key);
        if (!Enum.TryParse(text, ignoreCase: false, out T value) || !Enum.IsDefined(value) ||
            !string.Equals(value.ToString(), text, StringComparison.Ordinal))
        {
            throw new DescriptorException(path,
                $"'{key}' value '{text}' is not one of: {string.Join(", ", Enum.GetNames<T>())}.");
        }

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
            {
                Visit(Path.GetFullPath(root), found);
            }
        }

        return found.Values.OrderBy(plugin => plugin.Name, StringComparer.Ordinal).ToList();
    }

    /// <summary>
    /// 참조 목록(references — 프로젝트 기술자의 plugins, 타깃 규칙의 Plugins, 또는 둘을 합친 것)으로 켤 플러그인을 의존 순서(의존하는
    /// 플러그인이 뒤)로 정한다(Docs/Design/BuildTargets 3.9절).
    /// 1) enabled: true로 적은 플러그인을 켠다. applyDefaults면 적지 않은 플러그인 중 enabledByDefault인 것도 켠다(Game 타깃 — Program
    ///    타깃은 명시한 것만, BD34). enabled: false로 적은 플러그인은 기본값과 관계없이 끈다.
    /// 2) 켠 플러그인의 의존 플러그인을 모두 켠다.
    /// 오류(DescriptorException): 참조·플러그인이 없는 플러그인을 참조, 켠 플러그인이 참조가 끈 플러그인에 의존, 의존 순환,
    /// 같은 이름의 참조가 둘. 오류 문구의 참조 주체와 파일은 referrer다. 사용 예:
    /// <code>
    /// var enabled = PluginCatalog.ResolveEnabled(project.Plugins, PluginReferrer.Of(project), available, applyDefaults: true, context);
    /// </code>
    /// context(Docs/Design/BuildTargets 3.6절 BS4)가 있으면 조건 있는 참조는 문맥이 맞을 때만 그 enabled이고 아니면 끈 것과 같다(이름을
    /// 차지하므로 enabledByDefault도 켜지 않는다). 이번 플랫폼을 지원하지 않는 플러그인(PluginDescriptor.Platforms)은 기본 활성으로만 켜질
    /// 것이면 조용히 빼고, 참조가 켰거나 의존으로 요구되면 오류다. 문맥은 필수다(빠뜨리면 조건을 보지 않고 켜는 일이 없게).
    /// </summary>
    public static IReadOnlyList<PluginDescriptor> ResolveEnabled(IReadOnlyList<PluginReference> references, PluginReferrer referrer,
        IReadOnlyList<PluginDescriptor> available, bool applyDefaults, PluginContext context)
    {
        var byName = new Dictionary<string, PluginDescriptor>(StringComparer.Ordinal);
        foreach (PluginDescriptor plugin in available)
        {
            if (!byName.TryAdd(plugin.Name, plugin))
            {
                throw new DescriptorException(plugin.FilePath, $"duplicate plugin '{plugin.Name}'.");
            }
        }

        var explicitChoice = new Dictionary<string, bool>(StringComparer.Ordinal);
        // 조건이 문맥에 맞지 않아 꺼진 참조다(오류 문구가 조건을 안내한다).
        var offByCondition = new Dictionary<string, PluginReference>(StringComparer.Ordinal);
        foreach (PluginReference reference in references)
        {
            if (!byName.ContainsKey(reference.Name))
            {
                throw new DescriptorException(referrer.FilePath, $"{referrer.Label} references unknown plugin '{reference.Name}'.");
            }

            bool matches = reference.Matches(context);
            if (!explicitChoice.TryAdd(reference.Name, reference.Enabled && matches))
            {
                throw new DescriptorException(referrer.FilePath, $"{referrer.Label} references plugin '{reference.Name}' more than once.");
            }

            if (reference.Enabled && !matches)
            {
                offByCondition.Add(reference.Name, reference);
            }
        }

        // 이번 플랫폼을 지원하는가(BD13 — 문맥 없으면 늘 참).
        bool Supported(PluginDescriptor plugin) => plugin.Platforms is null || plugin.Platforms.Contains(context.Platform);

        // 1) 처음 켤 플러그인.
        var enabled = new HashSet<string>(StringComparer.Ordinal);
        foreach (PluginDescriptor plugin in byName.Values)
        {
            bool explicitly = explicitChoice.TryGetValue(plugin.Name, out bool choice);
            bool on = explicitly ? choice : applyDefaults && plugin.EnabledByDefault;
            if (on && !Supported(plugin))
            {
                if (!explicitly)
                {
                    continue; // 기본 활성으로만 켜질 플러그인은 지원하지 않는 플랫폼에서 조용히 뺀다.
                }

                throw new DescriptorException(referrer.FilePath, $"{referrer.Label} enables plugin '{plugin.Name}', which does not support " +
                    $"{context.Platform} (platforms in {plugin.FilePath}); add a platforms condition to the reference.");
            }

            if (on)
            {
                enabled.Add(plugin.Name);
            }
        }

        // 2) 의존을 따라 켜고, 깊이 우선으로 순서를 정하며 순환을 찾는다.
        var ordered = new List<PluginDescriptor>();
        var state = new Dictionary<string, bool>(StringComparer.Ordinal); // false=방문 중, true=완료
        foreach (string name in enabled.OrderBy(name => name, StringComparer.Ordinal).ToList())
        {
            Visit(name, requiredBy: null);
        }

        return ordered;

        void Visit(string name, PluginDescriptor? requiredBy)
        {
            if (!byName.TryGetValue(name, out PluginDescriptor? plugin))
            {
                throw new DescriptorException(requiredBy?.FilePath, $"plugin '{requiredBy?.Name}' depends on unknown plugin '{name}'.");
            }

            if (requiredBy is not null && explicitChoice.TryGetValue(name, out bool choice) && !choice)
            {
                throw new DescriptorException(referrer.FilePath, offByCondition.TryGetValue(name, out PluginReference? conditioned)
                    ? $"plugin '{requiredBy.Name}' requires '{name}', but the {referrer.Label} enables '{name}' only for " +
                        $"{conditioned.ConditionText} (this build: {context}); " + (explicitChoice.ContainsKey(requiredBy.Name)
                            ? $"give the reference that enables '{requiredBy.Name}' the same conditions."
                            : $"'{requiredBy.Name}' is enabled by default or as a dependency, so add a reference for it with the same conditions.")
                    : $"plugin '{requiredBy.Name}' requires '{name}', but the {referrer.Label} disables it.");
            }

            if (requiredBy is not null && !Supported(plugin))
            {
                throw new DescriptorException(plugin.FilePath,
                    $"plugin '{requiredBy.Name}' requires '{name}', which does not support {context.Platform}.");
            }

            if (state.TryGetValue(name, out bool done))
            {
                if (!done)
                {
                    throw new DescriptorException(plugin.FilePath, $"plugin dependency cycle reaches '{name}'.");
                }

                return;
            }

            state[name] = false;
            foreach (PluginReference dependency in plugin.Plugins.OrderBy(reference => reference.Name, StringComparer.Ordinal))
            {
                Visit(dependency.Name, plugin);
            }

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
            {
                throw new DescriptorException(plugin.FilePath,
                    $"duplicate plugin '{plugin.Name}' (also at {existing.FilePath}).");
            }

            found.Add(plugin.Name, plugin);
            return;
        }

        foreach (string child in Directory.EnumerateDirectories(directory).Order(StringComparer.Ordinal))
        {
            string childName = Path.GetFileName(child);
            if (childName is "bin" or "obj" or "out" or ".git")
            {
                continue;
            }

            if ((File.GetAttributes(child) & FileAttributes.ReparsePoint) != 0)
            {
                continue; // link를 따라가 같은 플러그인을 두 번 찾거나 저장소 밖을 읽지 않는다
            }

            Visit(child, found);
        }
    }
}
