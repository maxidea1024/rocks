using System.Text.Json;
using System.Text.Json.Nodes;

namespace Rocks.Tooling.Common;

/// <summary>
/// JSON5 읽기 오류 코드다. <see cref="Json5ErrorCode.NonFiniteNumber"/> 밖은 C++ <c>rocks::JsonErrorCode</c>(Core/Text/Json.h)와
/// 이름·뜻이 같고, 같은 입력에서 같은 코드가 나온다.
/// </summary>
public enum Json5ErrorCode
{
    /// <summary>입력이 <see cref="Json5ReadOptions.MaxInputBytes"/>(또는 1 GiB)를 넘는다.</summary>
    InputTooLarge,

    /// <summary>값·문자열·주석·컨테이너가 끝나기 전에 입력이 끝났다.</summary>
    UnexpectedEnd,

    /// <summary>그 자리에 올 수 없는 문자다(연속 쉼표, 짝 없는 '/', 식별자가 아닌 키 등).</summary>
    UnexpectedCharacter,

    /// <summary>true·false·null·Infinity·NaN이 아닌 단어다.</summary>
    InvalidLiteral,

    /// <summary>숫자 문법 오류다(앞자리 0, 지수 숫자 없음, 숫자 바로 뒤 식별자 글자 등).</summary>
    InvalidNumber,

    /// <summary>double로 표현할 수 없는 크기이거나 64비트 크기를 넘는 16진수다.</summary>
    NumberOutOfRange,

    /// <summary>금지된 이스케이프, 잘못된 \u·\x 16진수, 식별자 글자가 아닌 식별자 이스케이프다.</summary>
    InvalidEscape,

    /// <summary>잘못된 UTF-8이다(overlong, surrogate 부호화, 범위 밖, 잘림).</summary>
    InvalidUtf8,

    /// <summary>짝 없는 \uD800–\uDFFF 이스케이프다.</summary>
    UnpairedSurrogate,

    /// <summary>문자열 안의 이스케이프하지 않은 LF·CR이다.</summary>
    ControlCharacter,

    /// <summary>같은 객체 안의 같은 키다(JSON5는 뒤 값이 이기지만 Rocks는 오류다).</summary>
    DuplicateKey,

    /// <summary>최상위 값 뒤에 공백·주석이 아닌 내용이 있다.</summary>
    TrailingContent,

    /// <summary>중첩이 <see cref="Json5ReadOptions.MaxDepth"/>를 넘는다.</summary>
    DepthLimitExceeded,

    /// <summary>문자열·키의 UTF-8 길이가 <see cref="Json5ReadOptions.MaxStringBytes"/>를 넘는다.</summary>
    StringTooLong,

    /// <summary>값 수가 <see cref="Json5ReadOptions.MaxNodes"/>를 넘는다.</summary>
    TooManyNodes,

    /// <summary>
    /// C# 전용: <see cref="Json5Reader.ParseDocument"/>가 Infinity·NaN을 담은 문서를 받았다(<see cref="JsonDocument"/>는 비유한 수를
    /// 담을 수 없다). 문법 오류가 아니므로 <see cref="Json5Reader.Parse"/>는 이 코드를 내지 않는다.
    /// </summary>
    NonFiniteNumber,
}

/// <summary>
/// JSON5 읽기 한도다. 기본값은 C++ <c>rocks::JsonReadOptions</c>와 같고, 넘으면 해당 <see cref="Json5ErrorCode"/>다.
/// </summary>
public sealed record Json5ReadOptions
{
    /// <summary>입력 byte 수 상한이다(1 GiB를 넘게 줘도 1 GiB다).</summary>
    public long MaxInputBytes { get; init; } = 64L << 20;

    /// <summary>컨테이너 중첩 깊이 상한이다.</summary>
    public int MaxDepth { get; init; } = 128;

    /// <summary>문자열·키 하나의 UTF-8 byte 수 상한이다(이스케이프를 푼 뒤 기준).</summary>
    public long MaxStringBytes { get; init; } = 16L << 20;

    /// <summary>값(컨테이너 포함) 수 상한이다.</summary>
    public int MaxNodes { get; init; } = 4194304;
}

/// <summary>
/// JSON5 읽기 실패다. 위치는 C++ <c>rocks::JsonPosition</c>과 같은 규칙이다: line·column은 1부터, CRLF는 줄바꿈 1개, CR 단독도
/// 줄바꿈, U+2028·U+2029는 줄바꿈이 아니다(편집기·LSP 줄 번호와 같다), column은 code point 단위, 맨 앞 BOM은 세지 않는다.
/// </summary>
public sealed class Json5Exception : FormatException
{
    public Json5Exception(Json5ErrorCode code, int line, int column, long byteOffset)
        : base($"{line}:{column}: {Json5Reader.Message(code)}")
    {
        Code = code;
        Line = line;
        Column = column;
        ByteOffset = byteOffset;
    }

    /// <summary>오류 종류다.</summary>
    public Json5ErrorCode Code { get; }

    /// <summary>오류 위치의 줄(1부터)이다.</summary>
    public int Line { get; }

    /// <summary>오류 위치의 열(1부터, code point 단위)이다.</summary>
    public int Column { get; }

    /// <summary>오류 위치의 입력 byte 위치(건너뛴 BOM 포함)다.</summary>
    public long ByteOffset { get; }
}

/// <summary>
/// 사람이 손으로 고치는 설정 파일용 JSON5 1.0.0(https://spec.json5.org/) reader다. C++ Core의
/// <c>JsonDocument::Parse</c>(<c>JsonSyntax::Json5</c>)와 같은 입력 집합을 받고 같은 값·오류 코드·위치를 낸다 — 규칙과 Rocks 정책
/// 차이(중복 키·잘못된 UTF-8·짝 없는 surrogate는 오류, 16진수는 64비트 정수 전용, 숫자 토큰 정규화)는 Core/Text/Json.h의
/// <c>JsonReadOptions::syntax</c> 주석이 정본이다. 식별자·공백의 Unicode 범주는 Core와 같은 UCD로 만든 표
/// (Json5UnicodeTables.generated.cs)로 판정한다. 쓰기는 하지 않는다(System.Text.Json의 엄격한 writer를 쓴다).
/// </summary>
/// <example>
/// <code>
/// byte[] bytes = File.ReadAllBytes(path);
/// try
/// {
///     using JsonDocument document = Json5Reader.ParseDocument(bytes);
///     string name = document.RootElement.GetProperty("name").GetString()!;
/// }
/// catch (Json5Exception error)
/// {
///     Console.Error.WriteLine($"{path}:{error.Line}:{error.Column}: {error.Message}");
/// }
/// </code>
/// </example>
public static class Json5Reader
{
    /// <summary>
    /// JSON5 문서를 <see cref="JsonNode"/> 트리로 읽는다. 최상위 null이면 null을 돌려준다. 객체 멤버는 원문 순서다. 문자열은 이스케이프를
    /// 푼 값이다. 유한한 수는 RFC 8259 꼴로 정규화한 토큰(".5" → "0.5", "0x1F" → "31", 앞 '+' 제거)을 담은
    /// <see cref="JsonElement"/> 기반 <see cref="JsonValue"/>라 <c>GetValue&lt;int&gt;()</c>·<c>GetValue&lt;double&gt;()</c> 등으로 읽고,
    /// Infinity·-Infinity·NaN은 <c>JsonValue.Create(double)</c> 값이다(그대로 직렬화하면 System.Text.Json이 거부한다). 맨 앞 BOM 하나는
    /// 건너뛴다. 반환한 트리는 호출자가 소유한다.
    /// </summary>
    /// <param name="utf8">UTF-8 입력이다.</param>
    /// <param name="options">한도(없으면 기본값)다.</param>
    /// <returns>최상위 값이다.</returns>
    /// <exception cref="Json5Exception">문법·UTF-8·한도 오류다(첫 오류 하나, 위치 포함).</exception>
    public static JsonNode? Parse(ReadOnlySpan<byte> utf8, Json5ReadOptions? options = null)
    {
        return new Json5Parser(utf8, options ?? new Json5ReadOptions()).Run(out _);
    }

    /// <summary>
    /// JSON5 문서를 <see cref="JsonDocument"/>로 읽는다(<see cref="Parse"/>의 결과를 엄격한 JSON으로 옮긴 것이다). 기존 JsonElement
    /// 기반 코드가 그대로 쓸 수 있다. 반환한 문서는 호출자가 Dispose한다.
    /// </summary>
    /// <param name="utf8">UTF-8 입력이다.</param>
    /// <param name="options">한도(없으면 기본값)다.</param>
    /// <returns>읽은 문서다.</returns>
    /// <exception cref="Json5Exception">
    /// <see cref="Parse"/>의 오류, 또는 Infinity·NaN이 있으면 첫 비유한 수 위치의 <see cref="Json5ErrorCode.NonFiniteNumber"/>다.
    /// </exception>
    public static JsonDocument ParseDocument(ReadOnlySpan<byte> utf8, Json5ReadOptions? options = null)
    {
        Json5ReadOptions limits = options ?? new Json5ReadOptions();
        var parser = new Json5Parser(utf8, limits);
        JsonNode? root = parser.Run(out Json5Exception? nonFinite);
        if (nonFinite is not null)
        {
            throw nonFinite;
        }

        var buffer = new System.Buffers.ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { MaxDepth = limits.MaxDepth + 1 }))
        {
            if (root is null)
            {
                writer.WriteNullValue();
            }
            else
            {
                root.WriteTo(writer);
            }
        }

        return JsonDocument.Parse(buffer.WrittenMemory, new JsonDocumentOptions { MaxDepth = limits.MaxDepth + 1 });
    }

    /// <summary>오류 코드의 짧은 영어 설명이다(C++ <c>JsonErrorMessage</c>와 같은 문구).</summary>
    public static string Message(Json5ErrorCode code) => code switch
    {
        Json5ErrorCode.InputTooLarge => "input is larger than the read limit",
        Json5ErrorCode.UnexpectedEnd => "unexpected end of input",
        Json5ErrorCode.UnexpectedCharacter => "unexpected character",
        Json5ErrorCode.InvalidLiteral => "invalid literal (expected true, false or null)",
        Json5ErrorCode.InvalidNumber => "invalid number",
        Json5ErrorCode.NumberOutOfRange => "number is out of range",
        Json5ErrorCode.InvalidEscape => "invalid escape sequence",
        Json5ErrorCode.InvalidUtf8 => "invalid UTF-8",
        Json5ErrorCode.UnpairedSurrogate => "unpaired UTF-16 surrogate escape",
        Json5ErrorCode.ControlCharacter => "unescaped control character in string",
        Json5ErrorCode.DuplicateKey => "duplicate object key",
        Json5ErrorCode.TrailingContent => "content after the top-level value",
        Json5ErrorCode.DepthLimitExceeded => "nesting depth exceeds the read limit",
        Json5ErrorCode.StringTooLong => "string is longer than the read limit",
        Json5ErrorCode.TooManyNodes => "value count exceeds the read limit",
        Json5ErrorCode.NonFiniteNumber => "Infinity or NaN cannot be stored in a JsonDocument",
        _ => "unknown error",
    };
}
