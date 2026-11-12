using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Common;

/// <summary>원본 tarball 출처와 검증 방법이다.</summary>
public sealed record ThirdPartySource(string Url, string Sha256, string Verification);

/// <summary>선택한 라이선스와 원문 위치다. <see cref="File"/>은 라이브러리 폴더 기준 경로다.</summary>
public sealed record ThirdPartyLicense(string Spdx, string UpstreamExpression, string File, bool BinaryNoticeRequired);

/// <summary>원본에 있는 컴파일러 지시문 한 줄이다. <see cref="File"/>은 원본 디렉터리 기준, Line은 1부터다.</summary>
public sealed record ThirdPartyPragma(string File, int Line, string Text);

/// <summary>
/// 고정 버전 외부 라이브러리 manifest(<c>Core/Source/ThirdParty/&lt;Lib&gt;/ThirdParty.json</c>)다. 모듈 규칙과 정책 검사가
/// 같은 파일을 읽는 단일 출처다.
/// <list type="bullet">
/// <item><see cref="Files"/>·<see cref="PragmaDirectives"/>의 경로는 원본 디렉터리(<see cref="UpstreamDirectory"/>) 기준이다.</item>
/// <item><see cref="TranslationUnits"/>·<see cref="IncludeDirectories"/>·<see cref="AdapterHeaders"/>와 라이선스 파일 경로는
/// 라이브러리 폴더 기준이다(원본 파일 또는 Rocks 글루).</item>
/// <item>경로는 '/' 구분 상대 경로만 허용한다. 절대 경로·'..'·'.'·빈 조각·역슬래시는 오류다.</item>
/// </list>
/// 읽기는 엄격하다: 모르는 필드, 중복 필드, 잘못된 타입, 주석·끝 쉼표, 1 MiB 초과는 <see cref="FormatException"/>이다.
/// </summary>
public sealed record ThirdPartyManifest(
    string Name,
    string Version,
    string UpstreamDirectory,
    ThirdPartySource Source,
    ThirdPartyLicense License,
    IReadOnlyList<string> TranslationUnits,
    IReadOnlyList<string> IncludeDirectories,
    IReadOnlyList<string> Defines,
    IReadOnlyDictionary<string, IReadOnlyList<string>> PlatformDefines,
    IReadOnlyList<string> ConsumerDefines,
    IReadOnlyList<string> AdapterHeaders,
    IReadOnlyDictionary<string, string> Files,
    IReadOnlyList<ThirdPartyPragma> PragmaDirectives,
    IReadOnlyList<string> Notes)
{
    public const string FileName = "ThirdParty.json";

    /// <summary>외부 라이브러리 폴더의 부모다(저장소 루트 기준).</summary>
    public const string ThirdPartyRoot = "Core/Source/ThirdParty";

    /// <summary>platformDefines의 키로 허용하는 플랫폼 이름이다(NativePlatform 열거자 이름과 같다).</summary>
    public static readonly IReadOnlyList<string> PlatformNames = ["Windows", "MacOS", "Android"];

    /// <summary>
    /// 원본에서 기록해야 하는 컴파일러 지시문이다. 진단에 영향을 줄 수 있는 모든 형태를 기록한다:
    /// <c>#pragma</c>(<c>#pragma once</c> 제외, 지시문 사이 주석 허용, MSVC 번호 형식 포함 — clang-cl은 일부 MSVC 경고
    /// 번호를 clang 진단에 매핑해 따른다), <c>_Pragma</c>·<c>__pragma</c>(매크로 안 포함), 경고·deprecation 억제 define.
    /// 줄은 <see cref="ScanPragmaDirectives"/>가 줄 이음(<c>\</c>+개행)을 합친 논리 줄 단위로 검사한다.
    /// </summary>
    public static readonly Regex PragmaDirective = new(
        "^\\s*#\\s*(?:/\\*.*?\\*/\\s*)*pragma\\b(?!\\s+once\\s*(?://.*|/\\*.*)?$)" +
        "|\\b_Pragma\\b|\\b__pragma\\b" +
        "|^\\s*#\\s*(?:/\\*.*?\\*/\\s*)*define\\s+_(?:CRT|SCL)_\\w*NO_\\w*(?:WARNINGS|DEPRECATE)\\w*\\b",
        RegexOptions.CultureInvariant | RegexOptions.Compiled, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 원본 텍스트에서 <see cref="PragmaDirective"/>에 해당하는 논리 줄을 찾는다. UTF-8 BOM은 무시하고, 줄 이음은 다음
    /// 물리 줄과 합친다. 결과의 Line은 논리 줄이 시작하는 물리 줄(1부터), Text는 합친 논리 줄의 앞뒤 공백을 지운 값이다.
    /// 정책 검사(third-party)와 manifest 갱신 도구가 같은 함수를 써서 기록과 검사가 어긋나지 않는다.
    /// </summary>
    public static IReadOnlyList<ThirdPartyPragma> ScanPragmaDirectives(string file, byte[] bytes)
    {
        string text = new UTF8Encoding(false, false).GetString(bytes);
        if (text.StartsWith('﻿'))
            text = text[1..];
        string[] lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var found = new List<ThirdPartyPragma>();
        for (int index = 0; index < lines.Length; ++index)
        {
            int start = index;
            var logical = new StringBuilder(lines[index]);
            while (logical.Length > 0 && logical[^1] == '\\' && index + 1 < lines.Length)
            {
                logical.Length -= 1;
                logical.Append(lines[++index]);
            }
            // 줄 앞의 BOM(U+FEFF)은 .NET 정규식의 공백이 아니므로 지운다.
            string line = logical.ToString().TrimStart('﻿');
            if (PragmaDirective.IsMatch(line))
                found.Add(new(file, start + 1, line.Trim()));
        }
        return found;
    }

    private const int MaximumBytes = 1 << 20;
    private static readonly Regex Sha256Text = new("^[0-9a-f]{64}$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
    private static readonly Regex DefineName = new("^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static readonly string[] TopLevel =
    [
        "schema", "name", "version", "upstreamDirectory", "source", "license", "translationUnits", "includeDirectories",
        "defines", "platformDefines", "consumerDefines", "adapterHeaders", "files", "pragmaDirectives", "notes"
    ];

    /// <summary>
    /// 라이브러리 폴더의 manifest를 읽는다. 플랫폼별 정의는 <see cref="DefinesFor"/>로 합친다.
    /// </summary>
    /// <exception cref="FormatException">파일이 없거나 형식·값이 계약과 다르다.</exception>
    public static ThirdPartyManifest Read(string libraryDirectory)
    {
        string path = Path.Combine(libraryDirectory, FileName);
        if (!System.IO.File.Exists(path))
            throw new FormatException($"{path}: missing third-party manifest");
        byte[] bytes = System.IO.File.ReadAllBytes(path);
        if (bytes.Length > MaximumBytes)
            throw new FormatException($"{path}: manifest exceeds {MaximumBytes} bytes");
        string text;
        try
        {
            text = new UTF8Encoding(false, true).GetString(bytes);
        }
        catch (DecoderFallbackException error)
        {
            throw new FormatException($"{path}: manifest is not valid UTF-8", error);
        }

        try
        {
            using JsonDocument document = JsonDocument.Parse(text, new JsonDocumentOptions
            {
                AllowTrailingCommas = false, CommentHandling = JsonCommentHandling.Disallow, MaxDepth = 8
            });
            return Parse(document.RootElement);
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException)
        {
            throw new FormatException($"{path}: {error.Message}", error);
        }
    }

    /// <summary>모든 플랫폼 공통 정의 뒤에 해당 플랫폼 정의를 붙인다. 모르는 플랫폼 이름은 오류다.</summary>
    public IReadOnlyList<string> DefinesFor(string platform)
    {
        if (!PlatformNames.Contains(platform, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown platform '{platform}'.", nameof(platform));
        return PlatformDefines.TryGetValue(platform, out IReadOnlyList<string>? extra) ? [.. Defines, .. extra] : Defines;
    }

    private static ThirdPartyManifest Parse(JsonElement root)
    {
        Dictionary<string, JsonElement> fields = Object(root, "manifest", TopLevel);
        if (Integer(fields["schema"], "schema") != 1)
            throw new FormatException("schema: unsupported version (expected 1)");

        Dictionary<string, JsonElement> source = Object(fields["source"], "source", ["url", "sha256", "verification"]);
        string sourceUrl = Text(source["url"], "source.url");
        if (!sourceUrl.StartsWith("https://", StringComparison.Ordinal))
            throw new FormatException("source.url: must be an https URL");
        Dictionary<string, JsonElement> license = Object(fields["license"], "license",
            ["spdx", "upstreamExpression", "file", "binaryNoticeRequired"]);

        string upstream = RelativePath(Text(fields["upstreamDirectory"], "upstreamDirectory"), "upstreamDirectory");
        if (upstream.Contains('/'))
            throw new FormatException("upstreamDirectory: must be a single directory name");

        var platformDefines = new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
        Dictionary<string, JsonElement> platforms = Object(fields["platformDefines"], "platformDefines", PlatformNames, optional: true);
        foreach ((string platform, JsonElement value) in platforms)
            platformDefines.Add(platform, DefineList(value, "platformDefines." + platform));

        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var caseless = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, JsonElement> fileEntries = Object(fields["files"], "files", null);
        if (fileEntries.Count == 0)
            throw new FormatException("files: must list at least one file");
        foreach ((string file, JsonElement value) in fileEntries)
        {
            string hash = Text(value, "files." + file);
            if (!Sha256Text.IsMatch(hash))
                throw new FormatException($"files.{file}: SHA-256 must be 64 lowercase hex digits");
            // 대소문자만 다른 경로는 Windows·macOS 기본 파일 시스템에서 같은 파일이다.
            if (!caseless.Add(file))
                throw new FormatException($"files.{file}: duplicate path differing only by case");
            files.Add(RelativePath(file, "files"), hash);
        }

        var pragmas = new List<ThirdPartyPragma>();
        foreach (JsonElement entry in Array(fields["pragmaDirectives"], "pragmaDirectives"))
        {
            Dictionary<string, JsonElement> pragma = Object(entry, "pragmaDirectives[]", ["file", "line", "text"]);
            int line = Integer(pragma["line"], "pragmaDirectives[].line");
            if (line < 1)
                throw new FormatException("pragmaDirectives[].line: must be positive");
            pragmas.Add(new(RelativePath(Text(pragma["file"], "pragmaDirectives[].file"), "pragmaDirectives[].file"), line,
                Text(pragma["text"], "pragmaDirectives[].text")));
        }

        string sha256 = Text(source["sha256"], "source.sha256");
        if (!Sha256Text.IsMatch(sha256))
            throw new FormatException("source.sha256: must be 64 lowercase hex digits");

        IReadOnlyList<string> translationUnits = Paths(fields["translationUnits"], "translationUnits", nonEmpty: true);
        foreach (string unit in translationUnits)
            if (!unit.EndsWith(".c", StringComparison.Ordinal))
                throw new FormatException($"translationUnits: '{unit}' must be a .c file (third-party sources build as C17)");

        IReadOnlyList<string> defines = DefineList(fields["defines"], "defines");
        IReadOnlyList<string> consumerDefines = DefineList(fields["consumerDefines"], "consumerDefines");
        var libraryValues = defines.ToDictionary(DefineNameOf, define => define, StringComparer.Ordinal);
        foreach ((string platform, IReadOnlyList<string> extra) in platformDefines)
            foreach (string define in extra)
                if (libraryValues.ContainsKey(DefineNameOf(define)))
                    throw new FormatException($"platformDefines.{platform}: '{DefineNameOf(define)}' is already in defines");
        // adapter와 라이브러리가 같은 이름을 쓰면 값도 같아야 한다(예: Z_U4가 다르면 ABI가 소리 없이 어긋난다).
        foreach (string define in consumerDefines)
            if (libraryValues.TryGetValue(DefineNameOf(define), out string? libraryDefine) && libraryDefine != define)
                throw new FormatException($"consumerDefines: '{define}' differs from library define '{libraryDefine}'");
        return new(
            Text(fields["name"], "name"),
            Text(fields["version"], "version"),
            upstream,
            new(sourceUrl, sha256, Text(source["verification"], "source.verification")),
            new(Text(license["spdx"], "license.spdx"), Text(license["upstreamExpression"], "license.upstreamExpression"),
                RelativePath(Text(license["file"], "license.file"), "license.file"),
                Boolean(license["binaryNoticeRequired"], "license.binaryNoticeRequired")),
            translationUnits,
            Paths(fields["includeDirectories"], "includeDirectories", nonEmpty: false),
            defines,
            platformDefines,
            consumerDefines,
            Paths(fields["adapterHeaders"], "adapterHeaders", nonEmpty: true),
            files,
            pragmas,
            Strings(fields["notes"], "notes"));
    }

    private static Dictionary<string, JsonElement> Object(JsonElement element, string name, IReadOnlyList<string>? allowed,
        bool optional = false)
    {
        if (element.ValueKind != JsonValueKind.Object)
            throw new FormatException($"{name}: must be an object");
        var result = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (allowed != null && !allowed.Contains(property.Name, StringComparer.Ordinal))
                throw new FormatException($"{name}: unknown field '{property.Name}'");
            if (!result.TryAdd(property.Name, property.Value))
                throw new FormatException($"{name}: duplicate field '{property.Name}'");
        }
        if (allowed != null && !optional)
            foreach (string required in allowed)
                if (!result.ContainsKey(required))
                    throw new FormatException($"{name}: missing field '{required}'");
        return result;
    }

    private static string Text(JsonElement element, string name)
    {
        if (element.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(element.GetString()))
            throw new FormatException($"{name}: must be a non-empty string");
        string value = element.GetString()!;
        if (value.IndexOfAny(['\0', '\r', '\n']) >= 0)
            throw new FormatException($"{name}: must not contain NUL or line breaks");
        return value;
    }

    private static int Integer(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out int value)
            ? value
            : throw new FormatException($"{name}: must be a 32-bit integer");

    private static bool Boolean(JsonElement element, string name) => element.ValueKind switch
    {
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        _ => throw new FormatException($"{name}: must be a boolean")
    };

    private static IEnumerable<JsonElement> Array(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Array ? element.EnumerateArray() : throw new FormatException($"{name}: must be an array");

    private static IReadOnlyList<string> Strings(JsonElement element, string name) =>
        Array(element, name).Select(value => Text(value, name + "[]")).ToArray();

    private static IReadOnlyList<string> Paths(JsonElement element, string name, bool nonEmpty)
    {
        string[] paths = Strings(element, name).Select(value => RelativePath(value, name)).ToArray();
        if (nonEmpty && paths.Length == 0)
            throw new FormatException($"{name}: must not be empty");
        if (paths.Distinct(StringComparer.OrdinalIgnoreCase).Count() != paths.Length)
            throw new FormatException($"{name}: duplicate entry");
        return paths;
    }

    private static string DefineNameOf(string define) => define.Split('=')[0];

    private static IReadOnlyList<string> DefineList(JsonElement element, string name)
    {
        IReadOnlyList<string> defines = Strings(element, name);
        foreach (string define in defines)
        {
            if (!DefineName.IsMatch(DefineNameOf(define)))
                throw new FormatException($"{name}: invalid define '{define}'");
            // 정의 값으로 지시문을 주입하면 lexical 검사를 우회한다(adapter 번역 단위로도 전달된다).
            if (define.Contains("pragma", StringComparison.OrdinalIgnoreCase) ||
                define.Contains("SECURE_NO_", StringComparison.Ordinal) || define.Contains("NO_DEPRECATE", StringComparison.Ordinal))
                throw new FormatException($"{name}: define '{define}' must not inject compiler directives");
        }
        if (defines.Select(DefineNameOf).Distinct(StringComparer.Ordinal).Count() != defines.Count)
            throw new FormatException($"{name}: duplicate define");
        return defines;
    }

    // 경로 조각은 플랫폼 간 의미가 같은 문자만 허용한다. ':'(Windows 대체 데이터 스트림), 끝의 '.'·공백(Windows가
    // 잘라냄), '.'·'..', 빈 조각, 역슬래시, 절대 경로를 거부한다.
    private static readonly Regex PathSegment = new("^[A-Za-z0-9_+-][A-Za-z0-9._+-]*(?<![.])$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static string RelativePath(string value, string name)
    {
        if (value.StartsWith('/') || Path.IsPathRooted(value) || !value.Split('/').All(PathSegment.IsMatch))
            throw new FormatException($"{name}: '{value}' must be a relative '/' path of [A-Za-z0-9._+-] segments " +
                                      "without '.', '..', or a trailing dot");
        return value;
    }
}
