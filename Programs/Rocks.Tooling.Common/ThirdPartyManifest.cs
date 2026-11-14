using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Common;

/// <summary>원본 tarball 출처와 검증 방법이다.</summary>
public sealed record ThirdPartySource(string Url, string Sha256, string Verification);

/// <summary>
/// 선택한 라이선스 하나와 원문 위치다(manifest <c>licenses</c>의 항목). <see cref="File"/>은 라이브러리 폴더 기준 경로이고
/// 등록된 원본 파일이어야 한다(정책 검사 third-party). 한 라이브러리가 서로 다른 라이선스의 구성요소를 담으면(예: zstd의
/// BSD-3-Clause 본체와 MIT divsufsort) 항목을 여럿 둔다.
/// </summary>
/// <param name="Spdx">선택한 라이선스의 SPDX 식별자(예: <c>BSD-3-Clause</c>).</param>
/// <param name="UpstreamExpression">upstream이 밝힌 라이선스 표현식과 선택 근거.</param>
/// <param name="File">원문 파일 경로(라이브러리 폴더 기준). 원문이 헤더 주석에 있으면 그 원본 파일을 가리킨다.</param>
/// <param name="BinaryNoticeRequired">바이너리 배포 시 고지 의무가 있는지다.</param>
/// <param name="LicenseSource">원문이 원본 archive 밖에서 왔을 때의 출처다(manifest 항목의 선택 필드 <c>licenseSource</c>).
/// 파일 자체는 원본 디렉터리에 두고 <see cref="ThirdPartyManifest.Files"/>에 등록한다.</param>
public sealed record ThirdPartyLicense(string Spdx, string UpstreamExpression, string File, bool BinaryNoticeRequired,
    ThirdPartyFileSource? LicenseSource = null);

/// <summary>
/// 외부 라이브러리의 종류다. Code는 C 번역 단위를 빌드하는 라이브러리이고, Data는 생성기·시험이 읽는 데이터 파일
/// 묶음(예: Unicode UCD, JSON 시험 사례)이다. Data는 번역 단위·include·정의·adapter·지시문이 없고 모듈 규칙도 없다.
/// </summary>
public enum ThirdPartyKind { Code, Data }

/// <summary>원본 archive 밖에서 받은 파일(예: 따로 배포되는 라이선스 원문)의 출처 URL과 SHA-256이다.</summary>
public sealed record ThirdPartyFileSource(string Url, string Sha256);

/// <summary>원본에 있는 컴파일러 지시문 한 줄이다. <see cref="File"/>은 원본 디렉터리 기준, Line은 1부터다.</summary>
public sealed record ThirdPartyPragma(string File, int Line, string Text);

/// <summary>
/// 원본 디렉터리 파일의 추적 가능한 수정 한 건이다(manifest <c>modifications</c>). <see cref="File"/>은 원본 디렉터리 기준
/// 경로, <see cref="UpstreamSha256"/>은 수정 전 upstream 내용의 SHA-256이다. null이면 upstream에 없던 파일을 더한 것이다
/// (예: 생성 헤더를 미리 만든 판으로 둔 경우). <see cref="Reason"/>은 수정 이유다. 실제 차이는 manifest <c>patch</c>가 가리키는
/// git diff 형식 파일이 담고, 정책 검사가 거꾸로 적용해 upstream 해시와 대조한다(<see cref="ThirdPartyPatch"/>).
/// </summary>
public sealed record ThirdPartyModification(string File, string? UpstreamSha256, string Reason);

/// <summary>
/// 고정 버전 외부 라이브러리 manifest(<c>Core/Source/ThirdParty/&lt;Lib&gt;/ThirdParty.json</c>)다. 모듈 규칙과 정책 검사가
/// 같은 파일을 읽는 단일 출처다.
/// <list type="bullet">
/// <item><see cref="Files"/>·<see cref="PragmaDirectives"/>의 경로는 원본 디렉터리(<see cref="UpstreamDirectory"/>) 기준이다.</item>
/// <item><see cref="TranslationUnits"/>·<see cref="IncludeDirectories"/>·<see cref="AdapterHeaders"/>와 라이선스 파일 경로
/// (<see cref="Licenses"/>)는 라이브러리 폴더 기준이다(원본 파일 또는 Rocks 글루).</item>
/// <item>경로는 '/' 구분 상대 경로만 허용한다. 절대 경로·'..'·'.'·빈 조각·역슬래시는 오류다.</item>
/// </list>
/// 읽기는 엄격하다: 모르는 필드, 중복 필드, 잘못된 타입, 주석·끝 쉼표, 1 MiB 초과는 <see cref="FormatException"/>이다.
/// </summary>
public sealed record ThirdPartyManifest(
    string Name,
    string Version,
    string UpstreamDirectory,
    ThirdPartySource Source,
    IReadOnlyList<ThirdPartyLicense> Licenses,
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

    /// <summary>라이브러리 종류다(manifest <c>kind</c>, 생략하면 code).</summary>
    public ThirdPartyKind Kind { get; init; } = ThirdPartyKind.Code;

    /// <summary>
    /// data 종류의 파생물(생성 표 등)이 들어가는 모듈 이름이다(manifest <c>embeddedIn</c>). SDK 내보내기가 이 가운데 하나라도
    /// 담으면 라이선스 원문을 함께 배포한다. 비어 있으면 배포물에 들어가지 않는 데이터(시험 전용)다. code 종류는 모듈
    /// 자신이 라이브러리이므로 쓰지 않는다.
    /// </summary>
    public IReadOnlyList<string> EmbeddedIn { get; init; } = [];

    /// <summary>
    /// 라이브러리만 받는 컴파일 인자다(manifest <c>compileArguments</c>, 예: libsodium의 <c>-fno-strict-overflow</c>). 허용 목록
    /// <see cref="AllowedCompileArguments"/> 안의 GCC형 철자로 적는다. clang-cl도 같은 철자를 받으므로 모든 플랫폼에 그대로 넘긴다.
    /// </summary>
    public IReadOnlyList<string> CompileArguments { get; init; } = [];

    /// <summary>
    /// Release에서 이 라이브러리를 ThinLTO bitcode로 만들지 여부다(manifest <c>linkTimeOptimization</c>, 생략하면 true).
    /// false이면 일반 최적화 object로 남아 다른 번역 단위와 링크 때 섞여 최적화되지 않는다. upstream이 LTO 없이 검증한
    /// 코드 생성 형태를 지켜야 하는 라이브러리(상수 시간 암호 구현 등)에 쓴다.
    /// </summary>
    public bool LinkTimeOptimization { get; init; } = true;

    /// <summary>
    /// Asan 구성에서 이 라이브러리를 sanitizer로 계측할지 여부다(manifest <c>sanitizerInstrumentation</c>, 생략하면 true).
    /// false이면 계측 없이 컴파일해 계측한 엔진 코드와 링크한다. upstream이 sanitizer 계측을 지원하지 않는다고 밝힌
    /// 라이브러리(libsodium 설치 문서)에 쓴다. 라이브러리 안의 메모리 오류는 잡지 못하므로 사유를 notes에 적는다.
    /// </summary>
    public bool SanitizerInstrumentation { get; init; } = true;

    /// <summary>원본 디렉터리에 적용한 추적 가능한 수정 목록이다(manifest <c>modifications</c>). 비어 있으면 원본 그대로다.</summary>
    public IReadOnlyList<ThirdPartyModification> Modifications { get; init; } = [];

    /// <summary>
    /// <see cref="Modifications"/>의 차이를 담은 git diff 형식 파일이다(라이브러리 폴더 바로 아래, <c>.patch</c>). 수정이 있으면
    /// 반드시 있고, 없으면 null이다.
    /// </summary>
    public string? Patch { get; init; }

    /// <summary>외부 라이브러리 폴더의 부모다(저장소 루트 기준).</summary>
    public const string ThirdPartyRoot = "Core/Source/ThirdParty";

    /// <summary>platformDefines의 키로 허용하는 플랫폼 이름이다(NativePlatform 열거자 이름과 같다).</summary>
    public static readonly IReadOnlyList<string> PlatformNames = ["Windows", "MacOS", "Android"];

    /// <summary>
    /// platformDefines의 아키텍처 키 이름이다(NativeArchitecture 열거자 이름과 같다). 키는 <c>"Android.X64"</c>처럼
    /// 플랫폼 이름 뒤에 붙인다. 한 플랫폼이 여러 ABI를 빌드할 때(Android) SIMD 경로 정의를 나눈다.
    /// </summary>
    public static readonly IReadOnlyList<string> ArchitectureNames = ["X64", "Arm64"];

    /// <summary>
    /// compileArguments가 받는 인자다. 경고·대상·최적화·FP 모델을 바꾸지 않는 코드 생성 의미 인자만 둔다.
    /// <c>-fno-strict-overflow</c>는 clang에서 <c>-fwrapv</c>(clang 21부터 <c>-fwrapv-pointer</c>도)로 바뀐다. 둘을 함께 주면
    /// 하나가 "unused" 경고(-Werror)가 되므로 manifest는 하나만 고른다.
    /// </summary>
    public static readonly IReadOnlyList<string> AllowedCompileArguments = ["-fno-strict-aliasing", "-fno-strict-overflow", "-fwrapv"];

    private static readonly IReadOnlyList<string> PlatformDefineKeys =
        [.. PlatformNames, .. PlatformNames.SelectMany(platform => ArchitectureNames.Select(architecture => $"{platform}.{architecture}"))];

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
        {
            text = text[1..];
        }

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
            {
                found.Add(new(file, start + 1, line.Trim()));
            }
        }

        return found;
    }

    private const int MaximumBytes = 1 << 20;
    private static readonly Regex Sha256Text = new("^[0-9a-f]{64}$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
    private static readonly Regex DefineName = new("^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static readonly string[] TopLevel =
    [
        "schema", "name", "version", "upstreamDirectory", "source", "licenses", "translationUnits", "includeDirectories",
        "defines", "platformDefines", "consumerDefines", "adapterHeaders", "files", "pragmaDirectives", "notes"
    ];

    // 생략할 수 있는 최상위 필드다(기존 code manifest는 쓰지 않는다).
    private static readonly string[] OptionalTopLevel =
        ["kind", "embeddedIn", "compileArguments", "linkTimeOptimization", "sanitizerInstrumentation", "modifications", "patch"];

    // licenses 항목의 필수·선택 필드다.
    private static readonly string[] LicenseFields = ["spdx", "upstreamExpression", "file", "binaryNoticeRequired"];
    private static readonly string[] OptionalLicenseFields = ["licenseSource"];
    private static readonly Regex ModuleName = new("^[A-Za-z][A-Za-z0-9_]*$", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 라이브러리 폴더의 manifest를 읽는다. 플랫폼별 정의는 <see cref="DefinesFor"/>로 합친다.
    /// </summary>
    /// <exception cref="FormatException">파일이 없거나 형식·값이 계약과 다르다.</exception>
    public static ThirdPartyManifest Read(string libraryDirectory)
    {
        string path = Path.Combine(libraryDirectory, FileName);
        if (!System.IO.File.Exists(path))
        {
            throw new FormatException($"{path}: missing third-party manifest");
        }

        byte[] bytes = System.IO.File.ReadAllBytes(path);
        if (bytes.Length > MaximumBytes)
        {
            throw new FormatException($"{path}: manifest exceeds {MaximumBytes} bytes");
        }

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
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 8
            });
            return Parse(document.RootElement);
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException)
        {
            throw new FormatException($"{path}: {error.Message}", error);
        }
    }

    /// <summary>
    /// 모든 플랫폼 공통 정의, 해당 플랫폼 정의(<c>"Android"</c>), 플랫폼·아키텍처 정의(<c>"Android.X64"</c>) 순으로 붙인다.
    /// 모르는 플랫폼·아키텍처 이름은 오류다. 세 단계 사이에 같은 이름은 없다(리더가 거부한다).
    /// </summary>
    public IReadOnlyList<string> DefinesFor(string platform, string architecture)
    {
        if (!PlatformNames.Contains(platform, StringComparer.Ordinal))
        {
            throw new ArgumentException($"Unknown platform '{platform}'.", nameof(platform));
        }

        if (!ArchitectureNames.Contains(architecture, StringComparer.Ordinal))
        {
            throw new ArgumentException($"Unknown architecture '{architecture}'.", nameof(architecture));
        }

        IReadOnlyList<string> platformExtra = PlatformDefines.TryGetValue(platform, out IReadOnlyList<string>? extra) ? extra : [];
        IReadOnlyList<string> architectureExtra =
            PlatformDefines.TryGetValue($"{platform}.{architecture}", out IReadOnlyList<string>? specific) ? specific : [];
        return [.. Defines, .. platformExtra, .. architectureExtra];
    }

    /// <summary>SDK 안에서 외부 라이브러리 라이선스 원문을 두는 폴더다(<c>Licenses/&lt;Lib&gt;/</c>의 부모).</summary>
    public const string SdkLicenseDirectory = "Licenses";

    /// <summary>
    /// SDK 내보내기(Rocks.Build.Cli InstalledSdk)가 함께 배포할 외부 라이브러리 라이선스 원문을 <paramref name="destination"/>
    /// 아래로 복사한다. BSD 계열처럼 바이너리 배포에 고지 의무가 있는 원문을 정적 라이브러리와 함께 내보내려는 것이다.
    /// <list type="bullet">
    /// <item>code 라이브러리: SDK가 라이브러리 폴더 이름과 같은 모듈을 담으면 <see cref="Licenses"/>의 모든 원문을 복사한다.</item>
    /// <item>data 라이브러리: 모듈이 아니므로 <see cref="EmbeddedIn"/>의 모듈 가운데 하나라도 SDK에 있을 때만 복사한다.
    /// embeddedIn이 비어 있는 시험 전용 데이터(예: JsonTestSuite)는 배포하지 않는다.</item>
    /// <item>복사 위치는 <c>Licenses/&lt;Lib&gt;/&lt;원본 디렉터리 기준 경로&gt;</c>다. 예: zstd의
    /// <c>zstd-1.5.7/LICENSE</c>는 <c>Licenses/Zstd/LICENSE</c>, <c>zstd-1.5.7/lib/dictBuilder/divsufsort.h</c>는
    /// <c>Licenses/Zstd/lib/dictBuilder/divsufsort.h</c>다. 원본 안 상대 경로를 그대로 두므로 파일 이름이 같은 원문(최상위
    /// LICENSE와 하위 구성요소의 LICENSE)도 겹치지 않고, 어느 구성요소의 고지인지 경로로 드러난다. 경로가 겹칠 수 없는
    /// 근거는 manifest가 같은 원문 파일(대소문자 무시)을 두 번 적는 것을 거부한다는 점이다.</item>
    /// </list>
    /// 사용 예: <c>CopySdkLicenses(root, stage, packaged.Select(module =&gt; module.Name).ToArray())</c>.
    /// </summary>
    /// <param name="root">저장소 루트. <see cref="ThirdPartyRoot"/>가 없으면 아무것도 복사하지 않는다.</param>
    /// <param name="destination">SDK stage 루트. 같은 경로의 파일이 이미 있으면 덮어쓰지 않고 <see cref="IOException"/>이다.</param>
    /// <param name="modules">SDK가 담는 모듈 이름(대소문자 구분).</param>
    /// <returns>복사한 파일의 <paramref name="destination"/> 기준 '/' 상대 경로(라이브러리 폴더 이름 순, 폴더 안은 manifest 순).</returns>
    /// <exception cref="FormatException">manifest가 계약과 다르거나 라이선스 원문이 원본 디렉터리(<see cref="UpstreamDirectory"/>) 밖에 있다.</exception>
    public static IReadOnlyList<string> CopySdkLicenses(string root, string destination, IReadOnlyCollection<string> modules)
    {
        List<string> copied = [];
        string thirdParty = Path.Combine(root, ThirdPartyRoot);
        if (!Directory.Exists(thirdParty))
        {
            return copied;
        }

        foreach (string library in Directory.EnumerateDirectories(thirdParty).Order(StringComparer.Ordinal))
        {
            if (!System.IO.File.Exists(Path.Combine(library, FileName)))
            {
                continue;
            }

            string name = Path.GetFileName(library);
            ThirdPartyManifest manifest = Read(library);
            bool shipped = manifest.Kind == ThirdPartyKind.Code
                ? modules.Contains(name, StringComparer.Ordinal)
                : manifest.EmbeddedIn.Any(module => modules.Contains(module, StringComparer.Ordinal));
            if (!shipped)
            {
                continue;
            }

            string prefix = manifest.UpstreamDirectory + "/";
            foreach (ThirdPartyLicense license in manifest.Licenses)
            {
                // 정책 검사(third-party)도 같은 조건을 보지만, 검사를 거치지 않은 내보내기가 원본 밖 파일을 배포하지 않게 한다.
                if (!license.File.StartsWith(prefix, StringComparison.Ordinal))
                {
                    throw new FormatException($"{Path.Combine(library, FileName)}: license file '{license.File}' must be inside '{prefix}'");
                }

                string relative = $"{SdkLicenseDirectory}/{name}/{license.File[prefix.Length..]}";
                string target = Path.Combine(destination, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                System.IO.File.Copy(Path.Combine(library, license.File), target, overwrite: false);
                copied.Add(relative);
            }
        }

        return copied;
    }

    private static ThirdPartyManifest Parse(JsonElement root)
    {
        Dictionary<string, JsonElement> fields = Object(root, "manifest", TopLevel, optionalFields: OptionalTopLevel);
        if (Integer(fields["schema"], "schema") != 1)
        {
            throw new FormatException("schema: unsupported version (expected 1)");
        }

        ThirdPartyKind kind = ThirdPartyKind.Code;
        if (fields.TryGetValue("kind", out JsonElement kindElement))
        {
            kind = Text(kindElement, "kind") switch
            {
                "code" => ThirdPartyKind.Code,
                "data" => ThirdPartyKind.Data,
                _ => throw new FormatException("kind: must be \"code\" or \"data\"")
            };
        }

        bool data = kind == ThirdPartyKind.Data;

        Dictionary<string, JsonElement> source = Object(fields["source"], "source", ["url", "sha256", "verification"]);
        string sourceUrl = Text(source["url"], "source.url");
        if (!sourceUrl.StartsWith("https://", StringComparison.Ordinal))
        {
            throw new FormatException("source.url: must be an https URL");
        }

        IReadOnlyList<ThirdPartyLicense> licenses = LicenseList(fields["licenses"]);

        string upstream = RelativePath(Text(fields["upstreamDirectory"], "upstreamDirectory"), "upstreamDirectory");
        if (upstream.Contains('/'))
        {
            throw new FormatException("upstreamDirectory: must be a single directory name");
        }

        var platformDefines = new Dictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
        Dictionary<string, JsonElement> platforms = Object(fields["platformDefines"], "platformDefines", PlatformDefineKeys, optional: true);
        foreach ((string platform, JsonElement value) in platforms)
        {
            platformDefines.Add(platform, DefineList(value, "platformDefines." + platform));
        }

        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var caseless = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, JsonElement> fileEntries = Object(fields["files"], "files", null);
        if (fileEntries.Count == 0)
        {
            throw new FormatException("files: must list at least one file");
        }

        foreach ((string file, JsonElement value) in fileEntries)
        {
            string hash = Text(value, "files." + file);
            if (!Sha256Text.IsMatch(hash))
            {
                throw new FormatException($"files.{file}: SHA-256 must be 64 lowercase hex digits");
            }
            // 대소문자만 다른 경로는 Windows·macOS 기본 파일 시스템에서 같은 파일이다.
            if (!caseless.Add(file))
            {
                throw new FormatException($"files.{file}: duplicate path differing only by case");
            }

            files.Add(RelativePath(file, "files"), hash);
        }

        var pragmas = new List<ThirdPartyPragma>();
        foreach (JsonElement entry in Array(fields["pragmaDirectives"], "pragmaDirectives"))
        {
            Dictionary<string, JsonElement> pragma = Object(entry, "pragmaDirectives[]", ["file", "line", "text"]);
            int line = Integer(pragma["line"], "pragmaDirectives[].line");
            if (line < 1)
            {
                throw new FormatException("pragmaDirectives[].line: must be positive");
            }

            pragmas.Add(new(RelativePath(Text(pragma["file"], "pragmaDirectives[].file"), "pragmaDirectives[].file"), line,
                Text(pragma["text"], "pragmaDirectives[].text")));
        }

        string sha256 = Text(source["sha256"], "source.sha256");
        if (!Sha256Text.IsMatch(sha256))
        {
            throw new FormatException("source.sha256: must be 64 lowercase hex digits");
        }

        IReadOnlyList<string> translationUnits = Paths(fields["translationUnits"], "translationUnits", nonEmpty: !data);
        foreach (string unit in translationUnits)
        {
            if (!unit.EndsWith(".c", StringComparison.Ordinal))
            {
                throw new FormatException($"translationUnits: '{unit}' must be a .c file (third-party sources build as C17)");
            }
        }

        IReadOnlyList<string> defines = DefineList(fields["defines"], "defines");
        IReadOnlyList<string> consumerDefines = DefineList(fields["consumerDefines"], "consumerDefines");
        IReadOnlyList<string> includeDirectories = Paths(fields["includeDirectories"], "includeDirectories", nonEmpty: false);
        IReadOnlyList<string> adapterHeaders = Paths(fields["adapterHeaders"], "adapterHeaders", nonEmpty: !data);
        if (data)
        {
            // 데이터는 빌드하지 않으므로 빌드 입력이 있으면 계약 위반이다(빈 목록을 명시해 같은 schema를 유지한다).
            foreach ((string field, int count) in new[]
                     {
                         ("translationUnits", translationUnits.Count), ("includeDirectories", includeDirectories.Count),
                         ("defines", defines.Count), ("platformDefines", platformDefines.Count),
                         ("consumerDefines", consumerDefines.Count), ("adapterHeaders", adapterHeaders.Count),
                         ("pragmaDirectives", pragmas.Count)
                     })
            {
                if (count != 0)
                {
                    throw new FormatException($"{field}: must be empty for a data library");
                }
            }
        }

        IReadOnlyList<string> embeddedIn = [];
        if (fields.TryGetValue("embeddedIn", out JsonElement embeddedElement))
        {
            if (!data)
            {
                throw new FormatException("embeddedIn: only data libraries declare embedding modules (a code library is its own module)");
            }

            embeddedIn = Strings(embeddedElement, "embeddedIn");
            foreach (string module in embeddedIn)
            {
                if (!ModuleName.IsMatch(module))
                {
                    throw new FormatException($"embeddedIn: invalid module name '{module}'");
                }
            }

            if (embeddedIn.Distinct(StringComparer.OrdinalIgnoreCase).Count() != embeddedIn.Count)
            {
                throw new FormatException("embeddedIn: duplicate entry");
            }
        }

        var libraryValues = defines.ToDictionary(DefineNameOf, define => define, StringComparer.Ordinal);
        foreach ((string platform, IReadOnlyList<string> extra) in platformDefines)
        {
            // "Android.X64"는 공통 정의와 "Android" 정의 모두와 겹치면 안 된다(DefinesFor가 셋을 이어 붙인다).
            int dot = platform.IndexOf('.');
            IReadOnlyList<string> parent = dot > 0 && platformDefines.TryGetValue(platform[..dot], out IReadOnlyList<string>? found) ? found : [];
            foreach (string define in extra)
            {
                if (libraryValues.ContainsKey(DefineNameOf(define)))
                {
                    throw new FormatException($"platformDefines.{platform}: '{DefineNameOf(define)}' is already in defines");
                }

                if (parent.Any(item => DefineNameOf(item) == DefineNameOf(define)))
                {
                    throw new FormatException($"platformDefines.{platform}: '{DefineNameOf(define)}' is already in platformDefines.{platform[..dot]}");
                }
            }
        }

        IReadOnlyList<string> compileArguments = [];
        if (fields.TryGetValue("compileArguments", out JsonElement argumentsElement))
        {
            compileArguments = Strings(argumentsElement, "compileArguments");
            foreach (string argument in compileArguments)
            {
                if (!AllowedCompileArguments.Contains(argument, StringComparer.Ordinal))
                {
                    throw new FormatException($"compileArguments: '{argument}' is not allowed (allowed: {string.Join(", ", AllowedCompileArguments)})");
                }
            }

            if (compileArguments.Distinct(StringComparer.Ordinal).Count() != compileArguments.Count)
            {
                throw new FormatException("compileArguments: duplicate entry");
            }

            if (compileArguments.Contains("-fno-strict-overflow", StringComparer.Ordinal) &&
                compileArguments.Contains("-fwrapv", StringComparer.Ordinal))
            {
                throw new FormatException("compileArguments: '-fno-strict-overflow' already implies '-fwrapv' (clang rejects the pair as unused)");
            }
        }

        bool linkTimeOptimization = OptionalFalse(fields, "linkTimeOptimization");
        bool sanitizerInstrumentation = OptionalFalse(fields, "sanitizerInstrumentation");

        var modifications = new List<ThirdPartyModification>();
        if (fields.TryGetValue("modifications", out JsonElement modificationsElement))
        {
            var modified = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (JsonElement entry in Array(modificationsElement, "modifications"))
            {
                Dictionary<string, JsonElement> item = Object(entry, "modifications[]", ["file", "upstreamSha256", "reason"]);
                string file = RelativePath(Text(item["file"], "modifications[].file"), "modifications[].file");
                string? upstreamHash = null;
                if (item["upstreamSha256"].ValueKind != JsonValueKind.Null)
                {
                    upstreamHash = Text(item["upstreamSha256"], "modifications[].upstreamSha256");
                    if (!Sha256Text.IsMatch(upstreamHash))
                    {
                        throw new FormatException($"modifications[].upstreamSha256: '{file}' must be 64 lowercase hex digits or null (added file)");
                    }
                }

                string reason = Text(item["reason"], "modifications[].reason");
                if (reason.Trim().Length == 0)
                {
                    throw new FormatException($"modifications[].reason: '{file}' needs a reason");
                }

                if (!files.ContainsKey(file))
                {
                    throw new FormatException($"modifications[].file: '{file}' is not listed in files");
                }

                if (!modified.Add(file))
                {
                    throw new FormatException($"modifications[].file: '{file}' appears twice");
                }

                modifications.Add(new(file, upstreamHash, reason));
            }

            if (modifications.Count == 0)
            {
                throw new FormatException("modifications: omit the field instead of listing no modification");
            }
        }

        string? patch = null;
        if (fields.TryGetValue("patch", out JsonElement patchElement))
        {
            patch = RelativePath(Text(patchElement, "patch"), "patch");
            if (patch.Contains('/') || !patch.EndsWith(".patch", StringComparison.Ordinal))
            {
                throw new FormatException("patch: must be a .patch file directly in the library folder");
            }
        }

        if ((modifications.Count != 0) != (patch is not null))
        {
            throw new FormatException("modifications and patch must be declared together");
        }

        if (data && (compileArguments.Count != 0 || modifications.Count != 0 || !linkTimeOptimization || !sanitizerInstrumentation))
        {
            throw new FormatException(
                "compileArguments, linkTimeOptimization, sanitizerInstrumentation and modifications are not allowed for a data library");
        }
        // adapter와 라이브러리가 같은 이름을 쓰면 값도 같아야 한다(예: Z_U4가 다르면 ABI가 소리 없이 어긋난다).
        foreach (string define in consumerDefines)
        {
            if (libraryValues.TryGetValue(DefineNameOf(define), out string? libraryDefine) && libraryDefine != define)
            {
                throw new FormatException($"consumerDefines: '{define}' differs from library define '{libraryDefine}'");
            }
        }

        return new(
            Text(fields["name"], "name"),
            Text(fields["version"], "version"),
            upstream,
            new(sourceUrl, sha256, Text(source["verification"], "source.verification")),
            licenses,
            translationUnits,
            includeDirectories,
            defines,
            platformDefines,
            consumerDefines,
            adapterHeaders,
            files,
            pragmas,
            Strings(fields["notes"], "notes"))
        {
            Kind = kind,
            EmbeddedIn = embeddedIn,
            CompileArguments = compileArguments,
            LinkTimeOptimization = linkTimeOptimization,
            SanitizerInstrumentation = sanitizerInstrumentation,
            Modifications = modifications,
            Patch = patch
        };
    }

    // 기본이 true인 선택 bool 필드다. 기본값을 적는 것은 의미가 없어 false만 받는다. 없으면 true다.
    private static bool OptionalFalse(Dictionary<string, JsonElement> fields, string name)
    {
        if (!fields.TryGetValue(name, out JsonElement element))
        {
            return true;
        }

        if (element.ValueKind != JsonValueKind.False)
        {
            throw new FormatException($"{name}: only false is allowed (omit the field for the default)");
        }

        return false;
    }

    // licenses: 비어 있지 않은 배열이다. 항목마다 필수 4필드와 선택 licenseSource만 허용하고, 같은 원문 파일을 두 번 적지
    // 않는다(대소문자만 다른 경로도 Windows·macOS 기본 파일 시스템에서 같은 파일이다).
    private static IReadOnlyList<ThirdPartyLicense> LicenseList(JsonElement element)
    {
        var licenses = new List<ThirdPartyLicense>();
        var licenseFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement entry in Array(element, "licenses"))
        {
            string name = $"licenses[{licenses.Count}]";
            Dictionary<string, JsonElement> license = Object(entry, name, LicenseFields, optionalFields: OptionalLicenseFields);
            string file = RelativePath(Text(license["file"], name + ".file"), name + ".file");
            if (!licenseFiles.Add(file))
            {
                throw new FormatException($"{name}.file: '{file}' is already listed in licenses");
            }

            ThirdPartyFileSource? origin = null;
            if (license.TryGetValue("licenseSource", out JsonElement originElement))
            {
                Dictionary<string, JsonElement> fields = Object(originElement, name + ".licenseSource", ["url", "sha256"]);
                string url = Text(fields["url"], name + ".licenseSource.url");
                if (!url.StartsWith("https://", StringComparison.Ordinal))
                {
                    throw new FormatException($"{name}.licenseSource.url: must be an https URL");
                }

                string hash = Text(fields["sha256"], name + ".licenseSource.sha256");
                if (!Sha256Text.IsMatch(hash))
                {
                    throw new FormatException($"{name}.licenseSource.sha256: must be 64 lowercase hex digits");
                }

                origin = new(url, hash);
            }

            licenses.Add(new(Text(license["spdx"], name + ".spdx"), Text(license["upstreamExpression"], name + ".upstreamExpression"),
                file, Boolean(license["binaryNoticeRequired"], name + ".binaryNoticeRequired"), origin));
        }

        if (licenses.Count == 0)
        {
            throw new FormatException("licenses: must list at least one license");
        }

        return licenses;
    }

    // allowed는 필수 필드(optional이면 모두 선택), optionalFields는 있어도 되고 없어도 되는 필드다. 그 밖은 오류다.
    private static Dictionary<string, JsonElement> Object(JsonElement element, string name, IReadOnlyList<string>? allowed,
        bool optional = false, IReadOnlyList<string>? optionalFields = null)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            throw new FormatException($"{name}: must be an object");
        }

        var result = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (allowed != null && !allowed.Contains(property.Name, StringComparer.Ordinal) &&
                (optionalFields == null || !optionalFields.Contains(property.Name, StringComparer.Ordinal)))
            {
                throw new FormatException($"{name}: unknown field '{property.Name}'");
            }

            if (!result.TryAdd(property.Name, property.Value))
            {
                throw new FormatException($"{name}: duplicate field '{property.Name}'");
            }
        }

        if (allowed != null && !optional)
        {
            foreach (string required in allowed)
            {
                if (!result.ContainsKey(required))
                {
                    throw new FormatException($"{name}: missing field '{required}'");
                }
            }
        }

        return result;
    }

    private static string Text(JsonElement element, string name)
    {
        if (element.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(element.GetString()))
        {
            throw new FormatException($"{name}: must be a non-empty string");
        }

        string value = element.GetString()!;
        if (value.IndexOfAny(['\0', '\r', '\n']) >= 0)
        {
            throw new FormatException($"{name}: must not contain NUL or line breaks");
        }

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
        {
            throw new FormatException($"{name}: must not be empty");
        }

        if (paths.Distinct(StringComparer.OrdinalIgnoreCase).Count() != paths.Length)
        {
            throw new FormatException($"{name}: duplicate entry");
        }

        return paths;
    }

    private static string DefineNameOf(string define) => define.Split('=')[0];

    private static IReadOnlyList<string> DefineList(JsonElement element, string name)
    {
        IReadOnlyList<string> defines = Strings(element, name);
        foreach (string define in defines)
        {
            if (!DefineName.IsMatch(DefineNameOf(define)))
            {
                throw new FormatException($"{name}: invalid define '{define}'");
            }
            // 정의 값으로 지시문을 주입하면 lexical 검사를 우회한다(adapter 번역 단위로도 전달된다).
            if (define.Contains("pragma", StringComparison.OrdinalIgnoreCase) ||
                define.Contains("SECURE_NO_", StringComparison.Ordinal) || define.Contains("NO_DEPRECATE", StringComparison.Ordinal))
            {
                throw new FormatException($"{name}: define '{define}' must not inject compiler directives");
            }
        }

        if (defines.Select(DefineNameOf).Distinct(StringComparer.Ordinal).Count() != defines.Count)
        {
            throw new FormatException($"{name}: duplicate define");
        }

        return defines;
    }

    // 경로 조각은 플랫폼 간 의미가 같은 문자만 허용한다. ':'(Windows 대체 데이터 스트림), 끝의 '.'·공백(Windows가
    // 잘라냄), '.'·'..', 빈 조각, 역슬래시, 절대 경로를 거부한다.
    private static readonly Regex PathSegment = new("^[A-Za-z0-9_+-][A-Za-z0-9._+-]*(?<![.])$",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static string RelativePath(string value, string name)
    {
        if (value.StartsWith('/') || Path.IsPathRooted(value) || !value.Split('/').All(PathSegment.IsMatch))
        {
            throw new FormatException($"{name}: '{value}' must be a relative '/' path of [A-Za-z0-9._+-] segments " +
                                      "without '.', '..', or a trailing dot");
        }

        return value;
    }
}
