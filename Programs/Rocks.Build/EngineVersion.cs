using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Rocks.Build;

/// <summary>
/// 엔진 버전이다(Docs/Design/ReflectionFoundations.ko.md 8.2절, UE <c>Engine/Build/Build.version</c> 대응). 단일 출처는 저장소의
/// <c>Build/Build.version</c>(UTF-8 JSON 한 줄 <c>{ "MajorVersion": 0, "MinorVersion": 1, "PatchVersion": 0 }</c>)이고, Rocks.Build.Cli가
/// 실행마다 한 번 읽어 <see cref="NativeBuildRequest.EngineVersion"/>으로 넘긴다(설치 SDK 게임 빌드는 SDK manifest의 값). 빌드는 이
/// 값을 BuildInfo 생성 TU의 <c>rocks::build::CurrentEngineVersion()</c>과 도구 빌드 ID manifest의 <c>engine-version:</c> 줄에만 쓴다 —
/// 모듈 입력·<see cref="NativeBuildRequest.ContextFingerprint"/>·compile 정의에 넣지 않으므로 값을 올려도 각 실행 파일의 생성 TU와
/// link만 다시 돈다.
/// <para>
/// 사용 예:
/// <code>
/// EngineVersion version = EngineVersion.Read(EngineVersion.PathIn(root));   // 파일 오류는 InvalidDataException
/// string text = version.ToString();                                        // "0.1.0"
/// bool linkable = version.CanLinkProject(0, 1);                            // .rocksproject engineVersion "0.1"
/// </code>
/// </para>
/// 각 값은 0–65535다. 주·부가 모두 0인 값(0.0.x)은 버전이 아니다 — 0으로 초기화된 값이 호환 판정을 통과하지 않게 하려는 규칙이며
/// (UE도 0.0.0을 빈 값으로 본다) 생성자와 <see cref="Read"/>가 거부한다. <c>default(EngineVersion)</c>는 그런 빈 값이고
/// <see cref="IsEmpty"/>가 참이다. Patch는 호환 판정에 쓰지 않는다.
/// </summary>
public readonly record struct EngineVersion
{
    /// <summary>저장소 루트 기준 단일 출처 위치다.</summary>
    public const string RelativePath = "Build/Build.version";

    /// <summary>파일 크기 상한(byte)이다. 한 줄 JSON이므로 넘으면 잘못된 파일이다.</summary>
    public const int MaximumFileBytes = 1024;

    private static readonly string[] Keys = ["MajorVersion", "MinorVersion", "PatchVersion"];

    /// <summary>
    /// 버전을 만든다. 각 값이 0–65535 밖이면 ArgumentOutOfRangeException, 주·부가 모두 0이면 ArgumentException이다.
    /// </summary>
    public EngineVersion(int major, int minor, int patch)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(major);
        ArgumentOutOfRangeException.ThrowIfNegative(minor);
        ArgumentOutOfRangeException.ThrowIfNegative(patch);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(major, ushort.MaxValue);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(minor, ushort.MaxValue);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(patch, ushort.MaxValue);
        if (major == 0 && minor == 0)
        {
            throw new ArgumentException("Engine version major and minor must not both be 0 (0.0.x is the empty value).");
        }

        Major = (ushort)major;
        Minor = (ushort)minor;
        Patch = (ushort)patch;
    }

    public ushort Major { get; }

    public ushort Minor { get; }

    public ushort Patch { get; }

    /// <summary>주·부가 모두 0인 빈 값(<c>default(EngineVersion)</c>)인지다.</summary>
    public bool IsEmpty => Major == 0 && Minor == 0;

    /// <summary>저장소 루트 아래 단일 출처 파일의 절대 경로다.</summary>
    public static string PathIn(string root) => Path.Combine(Path.GetFullPath(root), "Build", "Build.version");

    /// <summary>
    /// <c>Build/Build.version</c>을 엄격하게 읽는다(기술자 JSON과 같은 규칙). 세 키(MajorVersion·MinorVersion·PatchVersion)가
    /// 정확히 한 번씩 있어야 하고 값은 0–65535 정수(10진 숫자만 — <c>1.0</c>·<c>1e0</c>·문자열은 아니다)다. 파일 없음·1 KiB 초과·
    /// UTF-8 아님·JSON 문법 오류·주석·후행 쉼표·모르는 키·중복 키·키 누락·범위 밖·주·부 모두 0은 경로를 담은
    /// InvalidDataException이다. 다른 값으로 대신하지 않는다.
    /// </summary>
    public static EngineVersion Read(string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists)
        {
            throw Error(path, $"the engine version file does not exist ({RelativePath} is the single engine version source).");
        }

        if (info.Length > MaximumFileBytes)
        {
            throw Error(path, $"the engine version file is larger than {MaximumFileBytes} bytes.");
        }

        string text;
        try
        {
            text = File.ReadAllText(path, new UTF8Encoding(false, true));
        }
        catch (DecoderFallbackException)
        {
            throw Error(path, "the engine version file is not valid UTF-8.");
        }

        return Parse(text, path);
    }

    /// <summary>
    /// 프로젝트 연결 판정이다(<c>.rocksproject</c>의 engineVersion "주.부"). 주·부가 같아야 연결한다 — Patch는 보지 않는다(UE
    /// 플러그인 EngineVersion 판정과 같은 형태). 패키지 로드 판정(더 새 주·부만 거부)과 의미가 달라 이름을 나눈다(C++
    /// <c>EngineVersion::CanLoadPackageCompatibleWith</c>).
    /// </summary>
    public bool CanLinkProject(ushort projectMajor, ushort projectMinor) => Major == projectMajor && Minor == projectMinor;

    /// <summary>정규 10진 표기 "주.부.패치"다(예: "0.1.0"). manifest의 <c>engine-version:</c> 줄과 같다.</summary>
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"{Major}.{Minor}.{Patch}");

    // 파일 내용 해석이다. 시험이 파일 없이 오류 규칙을 확인할 수 있게 경로와 분리한다.
    internal static EngineVersion Parse(string text, string path)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(text, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 4
            });
        }
        catch (JsonException error)
        {
            throw Error(path, "invalid JSON: " + error.Message);
        }

        using (document)
        {
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw Error(path, "the engine version root must be a JSON object.");
            }

            var values = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (JsonProperty property in root.EnumerateObject())
            {
                if (!Keys.Contains(property.Name, StringComparer.Ordinal))
                {
                    throw Error(path, $"unknown field '{property.Name}' (expected {string.Join(", ", Keys)}).");
                }

                if (values.ContainsKey(property.Name))
                {
                    throw Error(path, $"duplicate field '{property.Name}'.");
                }

                // JSON 수의 원문이 10진 숫자만이어야 한다(JSON은 앞자리 0을 이미 거부한다). 소수점·지수·부호가 있으면 정수가 아니다.
                string raw = property.Value.ValueKind == JsonValueKind.Number ? property.Value.GetRawText() : "";
                if (raw.Length == 0 || !raw.All(char.IsAsciiDigit) ||
                    !int.TryParse(raw, NumberStyles.None, CultureInfo.InvariantCulture, out int value) || value > ushort.MaxValue)
                {
                    throw Error(path, $"'{property.Name}' must be an integer from 0 to {ushort.MaxValue}.");
                }

                values.Add(property.Name, value);
            }

            foreach (string key in Keys)
            {
                if (!values.ContainsKey(key))
                {
                    throw Error(path, $"'{key}' is required.");
                }
            }

            if (values["MajorVersion"] == 0 && values["MinorVersion"] == 0)
            {
                throw Error(path, "MajorVersion and MinorVersion must not both be 0 (0.0.x is the empty value).");
            }

            return new EngineVersion(values["MajorVersion"], values["MinorVersion"], values["PatchVersion"]);
        }
    }

    private static InvalidDataException Error(string path, string message) => new($"{path}: {message}");
}
