#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Hash.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// RFC 8259 JSON 읽기·쓰기다(설계: Docs/Design/CoreDataFoundations.ko.md 3절, UE FJsonSerializer
// 대응이지만 엄격하다). 읽기는 사람이 고치는 파일용으로 JSON5 문법도 고를 수 있다
// (JsonReadOptions::syntax). 쓰기는 언제나 아래 엄격한 Canonical 규칙이다.
//
// 언제 쓰나: 버전 관리되는 설정(Config/*.json, TargetProfiles)·도구 보고서. 도구가 다시 쓴 파일의
// SHA-256이 쿠킹 키에 들어가므로 출력이 결정적이고(CRT·locale·부동소수점 환경 무관, byte 단위 고정
// 규칙), 사람이 고친 파일을 읽으므로 엄격하고 오류 위치가 정확하다.
//
// 읽기 규칙(기본 JsonSyntax::Strict): 입력은 UTF-8. 맨 앞 BOM 하나는 건너뛰고
// JsonWarningCode::BomIgnored를 남긴다. 주석(config 파일 읽기는 줄 주석만
// JsonReadOptions::allowLineComments로 받는다)·끝 쉼표·작은따옴표·NaN/Infinity 리터럴·잘못된
// UTF-8·짝 없는 surrogate 이스케이프·문자열 안 제어 문자·같은 객체 안 중복 키(이스케이프를 푼 byte
// 비교)는 오류다. 숫자: 소수부·지수가 없는 토큰은 int64, 넘으면 uint64, 그보다 크면 double이고
// 나머지는 double(정확한 반올림). double로도 표현할 수 없는 크기(1e400)는 NumberOutOfRange, -0은
// double −0.0. 원문 숫자 토큰도 보관한다.
//
// 사용 예:
//   JsonDocument document(allocator);
//   const JsonError error = document.Parse(text);
//   if (!error.Succeeded()) {
//       Report(error.position.line, error.position.column, JsonErrorMessage(error.code));
//   }
//   const JsonValue name = document.Root().Find("name");
//   if (name.IsString()) {
//       Use(name.GetString());
//   }
//
//   StringBuilder<> text(allocator);
//   JsonWriter writer(text);  // Canonical
//   bool ok = writer.BeginObject() && writer.Key("scale") && writer.Double(1.0) &&
//             writer.EndObject() && writer.Finish();
//   // ok이면 text == "{\n  \"scale\": 1.0\n}\n"
namespace rocks {

enum class JsonType : uint8 {
    Null,
    Bool,
    Integer,  // int64
    Unsigned, // int64 범위를 넘는 uint64
    Double,
    String,
    Array,
    Object
};

enum class JsonErrorCode : uint8 {
    None,
    InputTooLarge,       // JsonReadOptions::maxInputBytes(또는 1 GiB) 초과
    UnexpectedEnd,       // 값·문자열·컨테이너가 끝나기 전에 입력이 끝났다
    UnexpectedCharacter, // 그 자리에 올 수 없는 문자(Strict의 끝 쉼표·작은따옴표·주석 등)
    InvalidLiteral,      // true·false·null(Json5는 Infinity·NaN도)이 아닌 단어
    InvalidNumber,       // 숫자 문법 오류(앞자리 0, 소수점 뒤 숫자 없음, 숫자 뒤 식별자 글자 등)
    NumberOutOfRange,    // double로도 표현할 수 없는 크기, Json5 16진수의 64비트 크기 초과
    InvalidEscape,       // 알 수 없는·금지된 이스케이프, 잘못된 \u·\x 16진수, 식별자 글자가 아닌
                         // 식별자 이스케이프
    InvalidUtf8,         // 잘못된 UTF-8(overlong, surrogate 부호화, 범위 밖, 잘림)
    UnpairedSurrogate,   // 짝 없는 \uD800–\uDFFF
    ControlCharacter,    // 문자열 안의 이스케이프하지 않은 U+0000–U+001F(Json5는 LF·CR만)
    DuplicateKey,        // 같은 객체 안 같은 키
    TrailingContent,     // 최상위 값 뒤에 공백이 아닌 내용
    DepthLimitExceeded,  // JsonReadOptions::maxDepth 초과
    StringTooLong,       // JsonReadOptions::maxStringBytes 초과
    TooManyNodes,        // JsonReadOptions::maxNodes 초과
    OutOfMemory
};

// 읽기 문법이다(JsonReadOptions::syntax).
enum class JsonSyntax : uint8 {
    Strict, // RFC 8259(기본). 줄 주석만 JsonReadOptions::allowLineComments로 받는다.
    Json5   // JSON5 1.0.0 — 사람이 손으로 고치는 설정 파일용(JsonReadOptions::syntax 참고)
};

// 오류 코드의 짧은 영어 설명(정적 문자열)이다. syntax를 주면 그 문법에 맞는 문구다(Json5의
// InvalidLiteral은 Infinity·NaN도 든다 — C# Json5Reader.Message와 같은 문구).
//
// 사용 예:
//   const JsonError error = document.Parse(text, options);
//   Report(JsonErrorMessage(error.code, options.syntax));
ROCKS_CORE_API const char* JsonErrorMessage(JsonErrorCode code) noexcept;
ROCKS_CORE_API const char* JsonErrorMessage(JsonErrorCode code, JsonSyntax syntax) noexcept;

// 입력 위치. line·column은 1부터다. CRLF는 줄바꿈 1개, CR 단독도 줄바꿈이고, column은 code point
// 단위(tab은 1칸, 건너뛴 BOM은 세지 않는다)다. U+2028·U+2029는 Json5에서도 줄바꿈으로 세지 않는다
// (편집기·LSP의 줄 번호와 같다). 오류 위치는 잘못된 값·문자·이스케이프의 시작이다.
struct JsonPosition {
    uint32 line = 1;
    uint32 column = 1;
    uint64 byteOffset = 0;
};

struct JsonError {
    JsonErrorCode code = JsonErrorCode::None;
    JsonPosition position;

    bool Succeeded() const noexcept {
        return code == JsonErrorCode::None;
    }
};

enum class JsonWarningCode : uint8 {
    BomIgnored // 맨 앞 UTF-8 BOM을 건너뛰었다(RFC 8259 §8.1의 MAY)
};

struct JsonWarning {
    JsonWarningCode code;
    JsonPosition position;
};

// 읽기 한도와 읽기 규칙 옵션. 한도를 넘으면 해당 오류다. 깊이 한도는 JsonWriter의 기본 깊이 한도와
// 같다(더 깊게 읽은 문서는 같은 maxDepth를 준 writer로 쓴다). 한도는 두 문법에 똑같이 적용한다.
struct JsonReadOptions {
    uint64 maxInputBytes = 64ull << 20;
    uint32 maxDepth = 128;
    uint64 maxStringBytes = 16ull << 20;
    uint32 maxNodes = 4194304;
    // 줄 주석을 공백으로 받는다(사람이 고치는 config 파일 전용 — Docs/Design/ConfigSystem.ko.md
    // 4.1절). 값·키 사이 공백 자리의 `//`부터 줄 끝(LF·CR, 줄바꿈은 남는다) 또는 입력 끝까지를
    // 건너뛴다. 문자열 안의 `//`는 그대로 문자열이다. 주석 안도 UTF-8이어야 한다(아니면
    // InvalidUtf8 — 위치는 잘못된 byte). 블록 주석 `/*`, 짝 없는 `/`, 끝 쉼표, 중복 키는 이 옵션과
    // 무관하게 오류다. 기본은 거짓(엄격 — 쿠킹 manifest·도구 보고서 등)이다. 위치(line·column)는
    // 주석을 포함한 원문 기준이다.
    //
    // 사용 예:
    //   JsonReadOptions options;
    //   options.allowLineComments = true;
    //   document.Parse("{\n  // 그림자 품질\n  \"shadows\": 2\n}", options);
    bool allowLineComments = false;

    // 읽기 문법이다. 기본 Strict는 위 RFC 8259 규칙이다. Json5는 JSON5 1.0.0
    // (https://spec.json5.org/) 전체를 받는다 — 사람이 손으로 고치는 설정 파일에서 끝 쉼표·주석
    // 실수로 읽기가 깨지지 않게 하려는 것이다. Json5면 allowLineComments는 보지 않는다(주석은 늘
    // 받는다). 쓰기(JsonWriter)는 문법과 무관하게 엄격한 JSON이다.
    //
    // Json5가 더 받는 것:
    // - 객체·배열 끝 쉼표 하나(`[1, 2,]`). 빈 컨테이너의 쉼표(`[,]`)·연속 쉼표는 오류다.
    // - 주석 `//`(줄 끝 LF·CR·U+2028·U+2029 또는 입력 끝까지)·`/* */`(겹치지 않음, 닫히지 않으면
    //   UnexpectedEnd). 짝 없는 `/`는 UnexpectedCharacter다.
    // - 공백: SP·TAB·LF·CR·VT·FF·NBSP·U+2028·U+2029·U+FEFF(어디서나)와 Unicode Zs 범주.
    // - 키: 큰·작은따옴표 문자열 또는 ECMAScript 5.1 IdentifierName — 시작 글자는 Lu·Ll·Lt·Lm·Lo·Nl
    //   범주·'$'·'_', 다음 글자는 거기에 Mn·Mc·Nd·Pc·ZWNJ·ZWJ. `\uXXXX` 이스케이프도 되며 푼 글자도
    //   그 자리의 식별자 글자여야 한다(아니면 InvalidEscape — surrogate 이스케이프도 받지 않는다).
    //   예약어(`null`, `true`)도 키가 된다. 범주는 Core의 Unicode 표(GetGeneralCategory, Unicode
    //   18.0.0)로 판정한다 — ES5.1이 가리키는 Unicode 3.0보다 넓다.
    // - 문자열: 작은따옴표(`'it\'s'`), 이스케이프 `\'` `\v` `\0`(뒤에 10진 숫자가 오면
    //   InvalidEscape) `\xHH`, 그 밖 글자 이스케이프는 그 글자 자체(`\a` → a, 10진 숫자
    //   `\1`–`\9`는 InvalidEscape), 줄 이음(백슬래시 + LF·CR·CRLF·U+2028·U+2029는 없는 것으로),
    //   이스케이프하지 않은 U+2028·U+2029와 LF·CR 밖의 제어 문자. 이스케이프하지 않은 LF·CR은
    //   ControlCharacter다. 길이 한도(maxStringBytes)는 이스케이프를 푼 값의 UTF-8 byte 수다.
    // - 숫자: 앞 `+`, 16진 정수(`0x1F`, 부호 가능), 앞·뒤 소수점(`.5`, `5.`), `Infinity`·
    //   `-Infinity`·`NaN`(부호 가능). 숫자 바로 뒤에 식별자 글자가 붙으면 InvalidNumber다(`1x`,
    //   `0x1g`). 단 `Infinity1`·`-Infinityx`·`+NaN1`처럼 단어 뒤에 붙으면 InvalidLiteral이다.
    //   앞자리 0(`01`)도 InvalidNumber다.
    //
    // 숫자 값과 원문 토큰(RawNumber): 유한한 수의 토큰은 Strict 문법이 같은 종류·값으로 읽는 RFC
    // 8259 꼴로 바꿔 보관한다 — 앞 `+`를 버리고 `.5` → `0.5`, `5.` → `5.0`, `5.e3` → `5.0e3`,
    // 16진수는 10진 정수(`-0x10` → `-16`, `-0x0` → `-0`)다. 그래서 종류 판정(int64 → uint64 →
    // double, `-0`은 double −0.0)·Preserve 쓰기·TryParseFloat(RawNumber())가 Strict와 똑같이
    // 동작한다. 16진수는 정수 전용이라 64비트 크기를 넘으면 반올림하지 않고
    // NumberOutOfRange다(JSON5는 double로 반올림한다 — Rocks 정책). `Infinity`·`-Infinity`·`NaN`은
    // Double(±무한대, NaN은 부호와 무관하게 양의 quiet NaN 비트 하나)이고 토큰은
    // "Infinity"·"-Infinity"·"NaN"이다. JSON에 없는 값이라 JsonWriter는 Canonical·Preserve 모두
    // InvalidArgument로 거부하고, JsonEquals는 비트로 비교한다(NaN끼리 같다). TryGetDouble은
    // 그대로 돌려주므로 유한한 값이 필요한 호출자가 검사한다.
    //
    // JSON5와 다른 Rocks 정책: 중복 키는 DuplicateKey(JSON5는 뒤 값이 이긴다), 잘못된 UTF-8은
    // InvalidUtf8(주석 안 포함), 짝 없는 surrogate 이스케이프는 UnpairedSurrogate, 위 16진수 범위,
    // 맨 앞 BOM은 Strict처럼 건너뛰고 BomIgnored 경고를 남긴다. 한도·오류 위치 규칙은 Strict와 같다
    // (위치는 주석·이스케이프를 포함한 원문 기준). C# 도구(Programs/Rocks.Tooling.Common의
    // Json5Reader)가 같은 입력 집합·오류 코드·위치로 읽는다.
    //
    // 사용 예:
    //   JsonReadOptions options;
    //   options.syntax = JsonSyntax::Json5;
    //   document.Parse("{\n  // 그림자 품질\n  shadows: 2,\n  name: 'High',\n}", options);
    JsonSyntax syntax = JsonSyntax::Strict;
};

class JsonDocument;

// 문서 안 값 하나를 가리키는 가벼운 핸들이다. 문서가 살아 있는 동안 쓴다. 문서를 다시 Parse하거나
// Clear하면 핸들이 무효가 되고(세대 번호로 판별) IsValid()가 false가 된다. 기본 생성 값과 없는 키의
// Find 결과도 무효다. 무효 값의 Is*는 false, Size()는 0이고 그 밖의 접근자는 Fatal이다.
class ROCKS_CORE_API JsonValue {
public:
    JsonValue() noexcept = default;

    bool IsValid() const noexcept;

    // 전제: IsValid().
    JsonType Type() const noexcept;
    bool IsNull() const noexcept;
    bool IsBool() const noexcept;
    bool IsNumber() const noexcept; // Integer·Unsigned·Double
    bool IsString() const noexcept;
    bool IsArray() const noexcept;
    bool IsObject() const noexcept;

    // 전제: IsBool().
    bool GetBool() const noexcept;
    // 정수 종류가 범위에 들면 그 값, 아니면 InvalidArgument(double은 정수여도 InvalidArgument —
    // 손실 변환을 막는다).
    Result<int64> TryGetInt64() const noexcept;
    Result<uint64> TryGetUInt64() const noexcept;
    // 모든 숫자 종류를 double로(정수 종류는 가장 가까운 double). 숫자가 아니면 InvalidArgument.
    Result<double> TryGetDouble() const noexcept;
    // 원문 숫자 토큰(예: "1.50", "-0"). Json5로 읽은 문서는 RFC 8259 꼴로 바꾼 토큰(".5"는 "0.5",
    // 16진수는 10진수)이고 비유한 값은 "Infinity"·"-Infinity"·"NaN"이다(JsonReadOptions::syntax).
    // 전제: IsNumber().
    StringView RawNumber() const noexcept;
    // 이스케이프를 푼 UTF-8 문자열. 전제: IsString(). 문서가 바뀌기 전까지 유효하다.
    StringView GetString() const noexcept;

    // 배열 원소 수 또는 객체 멤버 수(그 밖 0).
    usize Size() const noexcept;
    // 배열 원소(전제: IsArray(), index < Size()).
    JsonValue At(usize index) const noexcept;
    // 객체 멤버를 원문 순서로(전제: IsObject(), index < Size()).
    StringView KeyAt(usize index) const noexcept;
    JsonValue ValueAt(usize index) const noexcept;
    // 객체 멤버 조회(해시). 객체가 아니거나 키가 없으면 무효 값.
    JsonValue Find(StringView key) const noexcept;

    // 이 값이 입력에서 시작하는 위치다(JsonError의 위치와 같은 규칙: 문자열은 여는 '"', 컨테이너는
    // '{'·'[', 숫자는 부호 또는 첫 숫자, 리터럴은 첫 글자. byteOffset은 건너뛴 BOM을 포함한 입력
    // byte 위치다). 문법은 맞지만 뜻이 틀린 값(모르는 키·타입 불일치·범위 밖)을 사람이 고치도록
    // 위치를 보고할 때 쓴다. 객체 멤버는 값의 위치다(키 위치는 따로 없다 — 보통 같은 줄이다).
    // Parse가 한 번에 계산해 두므로 상수 시간이다. 전제: IsValid()(아니면 Fatal).
    //
    // 사용 예:
    //   const JsonValue scale = document.Root().Find("scale");
    //   if (scale.IsValid() && !scale.IsNumber()) {
    //       const JsonPosition at = scale.Position();
    //       Report(at.line, at.column, "scale must be a number");
    //   }
    JsonPosition Position() const noexcept;

private:
    friend class JsonDocument;
    friend ROCKS_CORE_API bool JsonEquals(JsonValue left, JsonValue right) noexcept;
    JsonValue(const JsonDocument* document, uint32 node, uint32 generation) noexcept
        : document_(document), node_(node), generation_(generation) {
    }

    void CheckValid(const char* operation) const noexcept;

    const JsonDocument* document_ = nullptr;
    uint32 node_ = 0;
    uint32 generation_ = 0;
};

// 두 값이 같은지(구조·키 순서·문자열 byte·숫자 값)를 정확히 본다(반올림 없음, 정수 연산만 —
// 부동소수점 환경과 무관). 숫자는 수치로 비교하므로 정수 1과 double 1.0, 0.0과 −0.0은 같다 — 같아도
// Canonical 출력 byte가 같다는 뜻은 아니다(1과 1.0). Json5의 ±Infinity·NaN은 비트로 비교한다(NaN은
// 한 비트 패턴으로 저장되므로 NaN끼리 같다). 깊이와 무관하게 재귀하지 않는다. 무효 값끼리는 같다.
ROCKS_CORE_API bool JsonEquals(JsonValue left, JsonValue right) noexcept;

// 읽은 JSON 문서다. 노드·문자열을 allocator의 평평한 배열 몇 개에 담는다. 복사·이동하지 않는다.
class ROCKS_CORE_API JsonDocument final {
public:
    explicit JsonDocument(Allocator& allocator) noexcept;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;

    // input을 읽어 문서 내용을 바꾼다. 성공하면 Root()가 최상위 값이고 Warnings()에 경고가 있다.
    // 실패하면 문서는 비고(Root() 무효) 첫 오류와 위치를 돌려준다. 이전 내용의 JsonValue는 모두
    // 무효가 된다. input이 이 문서의 문자열(GetString)이어도 된다(먼저 복사한다).
    JsonError Parse(StringView input, const JsonReadOptions& options = {}) noexcept;

    JsonValue Root() const noexcept {
        return nodes_.IsEmpty() ? JsonValue() : JsonValue(this, 0, generation_);
    }

    Span<const JsonWarning> Warnings() const noexcept;
    void Clear() noexcept;

private:
    friend class JsonValue;
    friend class JsonParser;
    friend ROCKS_CORE_API bool JsonEquals(JsonValue left, JsonValue right) noexcept;

    // 노드는 문서 순서(전위)로 쌓이므로 값 하나의 서브트리는 [자기 위치, end) 연속 구간이다.
    struct Node {
        JsonType type;
        bool boolean;
        uint32 first; // String·숫자 원문: text_ 위치, Array: elements_ 시작, Object: members_ 시작
        uint32 count; // String·숫자 원문: byte 수, Array·Object: 원소·멤버 수
        uint32 end;   // 서브트리 끝(마지막 후손 다음 노드 위치)
        uint64 bits;  // Integer·Unsigned·Double 값의 비트
    };

    struct Member {
        uint32 keyOffset;
        uint32 keyLength;
        uint32 value;
    };

    // 노드 시작 위치(nodes_와 같은 순서). 입력 상한이 1 GiB라 uint32로 충분하다.
    struct NodePosition {
        uint32 byteOffset;
        uint32 line;
        uint32 column;
    };

    // 중복 키 판정과 조회용 색인 키: (객체 노드, text_의 키 범위). 조회는 (객체 노드, StringView).
    // 키는 입력 파일에서 오므로 seed 해시(SeededHash — 프로세스 seed)로 색인한다(hash flooding
    // 방어, Docs/Design/ReflectionExtensions.ko.md 10.1절).
    struct MemberKey {
        uint32 object;
        uint32 offset;
        uint32 length;
    };

    struct MemberQuery {
        uint32 object;
        StringView key;
    };

    struct MemberHash {
        const Array<char>* text;
        SeededHash<StringView> bytes;
        uint64 operator()(const MemberKey& key) const noexcept;
        uint64 operator()(const MemberQuery& query) const noexcept;
    };

    struct MemberEqual {
        const Array<char>* text;
        bool operator()(const MemberKey& left, const MemberKey& right) const noexcept;
        bool operator()(const MemberKey& left, const MemberQuery& right) const noexcept;
    };

    StringView Text(uint32 offset, uint32 length) const noexcept;

    Array<Node> nodes_;
    Array<NodePosition> positions_;
    Array<uint32> elements_;
    Array<Member> members_;
    Array<char> text_;
    Array<JsonWarning> warnings_;
    HashMap<MemberKey, uint32, MemberHash, MemberEqual> index_;
    uint32 generation_ = 1;
};

// 숫자 쓰기 방식.
enum class JsonNumberMode : uint8 {
    // 정수 종류는 10진, double은 최단 왕복 표기(첫 유효 숫자의 10진 지수가 −4 이상 16 미만이면 고정
    // 소수점, 아니면 d.ddde±XX — 지수는 두 자리 이상)이며 '.'·'e'가 없으면 ".0"을 붙여 다시 읽어도
    // double이 된다(−0.0은 "-0.0"). 읽기 → 쓰기 → 읽기가 종류·비트까지 같은 DOM이고 쓰기 결과는
    // 고정점이다.
    Canonical,
    // DOM 값(Value)을 쓸 때 원문 숫자 토큰을 그대로 쓴다(사람이 쓴 표기를 유지하는 편집 도구용).
    // 원문은 파서 문법을 통과한 토큰이라 다시 읽으면 같은 DOM이고 출력도 같다.
    Preserve
};

// JsonWriter의 쓰기 옵션이다.
// - mode·maxDepth: 숫자 쓰기 방식과 최대 중첩 깊이(JsonReadOptions::maxDepth와 같은 기본값)
// - compact: 한 줄로 쓴다. 들여쓰기·줄바꿈이 없고 키 구분자는 ":"이다(JSON lines 출력).
//   Finish의 끝 줄바꿈은 그대로 쓴다.
// - fixedFrames: 컨테이너 중첩 상태를 writer 안의 고정 배열(JsonWriter::FixedFrameCapacity칸)에
//   둔다. 할당하지 않는 대신 깊이가 그 수로 제한된다(maxDepth와 둘 중 작은 값).
// - detectDuplicateKeys: 같은 객체의 중복 키를 거부한다(색인 HashMap을 할당한다). 끄면 확인하지
//   않으므로 호출자가 키가 겹치지 않음을 보장한다(로그 속성 이름은 템플릿 검증이 막는다).
// fixedFrames·detectDuplicateKeys 끔이고 출력 builder가 할당하지 않으면(FixedStringBuilder)
// String·Key·숫자·컨테이너 쓰기는 할당하지 않는다. Value(문서 값 복사)는 여전히 할당할 수 있다.
//
// 사용 예:
//   JsonWriterOptions options;
//   options.compact = true;
//   options.fixedFrames = true;
//   options.detectDuplicateKeys = false;
//   JsonWriter writer(line, options);
struct JsonWriterOptions {
    JsonNumberMode mode = JsonNumberMode::Canonical;
    uint32 maxDepth = 128;
    bool compact = false;
    bool fixedFrames = false;
    bool detectDuplicateKeys = true;
};

// StringBuilder에 JSON을 쓴다. byte 단위 고정 규칙(기본 옵션): 들여쓰기 2칸, 줄바꿈 LF, Finish가
// 끝에 줄바꿈 1개, 키 구분자 ": ", 빈 컨테이너 {}·[], 키 순서는 호출 순서. 문자열은 '"'·'\\'·제어
// 문자만 이스케이프한다(U+0008·0C·0A·0D·09는 \b\f\n\r\t, 그 밖 제어 문자는 소문자 \u00xx). 그 밖의
// 문자('/'·U+007F·U+2028 포함)는 UTF-8 그대로이며 입력 문자열의 UTF-8을 검증한다. BOM은 쓰지
// 않는다. 입력 문자열이 out의 View()여도 된다. 들여쓰기 때문에 출력 크기는 중첩 깊이의 제곱에
// 비례한다(깊이 D인 사슬은 약 2·D² byte) — 아주 깊은 문서는 maxDepth를 올려도 출력이 커진다.
//
// 오류: 구조 오류(키 없이 값, 같은 객체의 중복 키 — 자기 reader가 거부한다, 닫지 않은 컨테이너,
// Finish 뒤 호출 등)·NaN·Inf(Json5로 읽은 문서 값 포함, Preserve도)·잘못된 UTF-8·무효 JsonValue는
// InvalidArgument, maxDepth 초과는 CapacityExceeded, builder·색인 할당 실패는 OutOfMemory. 한 번
// 실패하면 이후 호출은 모두 실패하고 builder에는 실패 전까지 쓴 내용이 남는다(그 결과는 버린다).
// 복사하지 않는다.
class ROCKS_CORE_API JsonWriter final {
public:
    // maxDepth는 JsonReadOptions::maxDepth와 같은 기본값이다(쓴 결과를 기본 옵션으로 다시 읽을 수
    // 있다).
    explicit JsonWriter(StringBuilderBase& out, JsonNumberMode mode = JsonNumberMode::Canonical,
                        uint32 maxDepth = 128) noexcept;
    // 옵션을 함께 받는다(JsonWriterOptions 참고).
    JsonWriter(StringBuilderBase& out, const JsonWriterOptions& options) noexcept;

    // fixedFrames 옵션의 고정 중첩 칸 수다.
    static constexpr uint32 FixedFrameCapacity = 32;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    Result<void> BeginObject() noexcept;
    Result<void> EndObject() noexcept;
    Result<void> BeginArray() noexcept;
    Result<void> EndArray() noexcept;
    Result<void> Key(StringView key) noexcept;
    Result<void> Null() noexcept;
    Result<void> Bool(bool value) noexcept;
    Result<void> Int64(int64 value) noexcept;
    Result<void> UInt64(uint64 value) noexcept;
    Result<void> Double(double value) noexcept;
    // float 값을 단정도 최단 왕복 표기로 쓴다(0.1f는 "0.1" — Double(0.1f)는 넓힌 double의
    // "0.10000000149011612"다). 고정 소수점·과학 표기 경계와 ".0" 규칙은 Double과 같다(Canonical,
    // 1.0f는 "1.0", -0.0f는 "-0.0"). 다시 읽은 문서에서 같은 float를 얻으려면 TryGetDouble로
    // double을 거치지 말고 RawNumber()를 TryParseFloat(NumberText.h)로 읽는다(이중 반올림을
    // 피한다). NaN·Inf는 InvalidArgument다. 숫자 모드(Preserve)와 무관하게 이 표기를 쓴다.
    //
    // 사용 예:
    //   writer.Key("gamma") && writer.Float(2.2f);   // "gamma": 2.2
    //   // 읽기: TryParseFloat(document.Root().Find("gamma").RawNumber()) == 2.2f
    Result<void> Float(float value) noexcept;
    Result<void> String(StringView value) noexcept;
    // 문서 값을 통째로 쓴다(Preserve면 원문 숫자 토큰). 재귀하지 않는다.
    Result<void> Value(JsonValue value) noexcept;
    // 최상위 값 하나가 끝났는지 확인하고 끝 줄바꿈을 쓴다.
    Result<void> Finish() noexcept;

private:
    struct Frame {
        bool object;
        bool hasItems;
        uint32 serial; // 객체 일련번호(중복 키 색인용)
    };

    // 이미 쓴 키: (객체 일련번호, 출력 안 이스케이프된 키의 위치). 이스케이프가 결정적이므로
    // 이스케이프된 byte가 같으면 같은 키다.
    struct WrittenKey {
        uint32 object;
        usize offset;
        usize length;
    };

    // 쓴 키도 입력 문서에서 옮겨 온 것일 수 있어(Value) 문서 색인과 같은 seed 해시를 쓴다.
    struct WrittenKeyHash {
        const StringBuilderBase* out;
        SeededHash<StringView> bytes;
        uint64 operator()(const WrittenKey& key) const noexcept;
    };

    struct WrittenKeyEqual {
        const StringBuilderBase* out;
        bool operator()(const WrittenKey& left, const WrittenKey& right) const noexcept;
    };

    Result<void> BeforeValue() noexcept;
    Result<void> AfterValue() noexcept;
    Result<void> BeginContainer(bool object) noexcept;
    Result<void> EndContainer(bool object) noexcept;
    Result<void> WriteNumber(JsonValue value) noexcept;
    Result<void> FinishFloatingPoint(usize start) noexcept;
    Result<void> WriteEscaped(StringView text) noexcept;
    Result<void> WriteIndent(usize depth) noexcept;
    Result<void> Fail(ErrorCode code) noexcept;
    Result<void> Check(Result<void> result) noexcept;

    // 중첩 상태 접근(고정 배열 또는 Array)이다.
    usize FrameCount() const noexcept;
    Frame& TopFrame() noexcept;
    bool PushFrame(Frame frame) noexcept;
    void PopFrame() noexcept;

    StringBuilderBase* out_;
    JsonNumberMode mode_;
    uint32 maxDepth_;
    bool compact_ = false;
    bool fixedFrames_ = false;
    bool detectDuplicateKeys_ = true;
    uint32 fixedCount_ = 0;
    Frame fixed_[FixedFrameCapacity]{};
    Array<Frame> frames_;
    HashMap<WrittenKey, uint8, WrittenKeyHash, WrittenKeyEqual> keys_;
    uint32 nextSerial_ = 0;
    bool expectingValue_ = false; // 객체에서 Key 뒤
    bool rootWritten_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace rocks
