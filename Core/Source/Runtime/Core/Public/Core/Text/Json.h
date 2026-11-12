#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// RFC 8259 JSON 읽기·쓰기다(설계: Docs/Design/CoreDataFoundations.ko.md 3절, UE FJsonSerializer
// 대응이지만 엄격하다).
//
// 언제 쓰나: 버전 관리되는 설정(Config/*.json, TargetProfiles)·도구 보고서. 도구가 다시 쓴 파일의
// SHA-256이 쿠킹 키에 들어가므로 출력이 결정적이고(CRT·locale·부동소수점 환경 무관, byte 단위 고정
// 규칙), 사람이 고친 파일을 읽으므로 엄격하고 오류 위치가 정확하다.
//
// 읽기 규칙: 입력은 UTF-8. 맨 앞 BOM 하나는 건너뛰고 JsonWarningCode::BomIgnored를 남긴다. 주석·끝
// 쉼표·작은따옴표·NaN/Infinity 리터럴·잘못된 UTF-8·짝 없는 surrogate 이스케이프·문자열 안 제어
// 문자·같은 객체 안 중복 키(이스케이프를 푼 byte 비교)는 오류다. 숫자: 소수부·지수가 없는 토큰은
// int64, 넘으면 uint64, 그보다 크면 double이고 나머지는 double(정확한 반올림). double로도 표현할 수
// 없는 크기(1e400)는 NumberOutOfRange, -0은 double −0.0. 원문 숫자 토큰도 보관한다.
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
    UnexpectedCharacter, // 그 자리에 올 수 없는 문자(끝 쉼표, 작은따옴표, 주석 등)
    InvalidLiteral,      // true·false·null이 아닌 단어
    InvalidNumber,       // 숫자 문법 오류(앞자리 0, 소수점 뒤 숫자 없음 등)
    NumberOutOfRange,    // double로도 표현할 수 없는 크기
    InvalidEscape,       // 알 수 없는 이스케이프 또는 잘못된 \u 16진수
    InvalidUtf8,         // 잘못된 UTF-8(overlong, surrogate 부호화, 범위 밖, 잘림)
    UnpairedSurrogate,   // 짝 없는 \uD800–\uDFFF
    ControlCharacter,    // 문자열 안의 이스케이프하지 않은 U+0000–U+001F
    DuplicateKey,        // 같은 객체 안 같은 키
    TrailingContent,     // 최상위 값 뒤에 공백이 아닌 내용
    DepthLimitExceeded,  // JsonReadOptions::maxDepth 초과
    StringTooLong,       // JsonReadOptions::maxStringBytes 초과
    TooManyNodes,        // JsonReadOptions::maxNodes 초과
    OutOfMemory
};

// 오류 코드의 짧은 영어 설명(정적 문자열)이다.
const char* JsonErrorMessage(JsonErrorCode code) noexcept;

// 입력 위치. line·column은 1부터다. CRLF는 줄바꿈 1개, CR 단독도 줄바꿈이고, column은 code point
// 단위(tab은 1칸, 건너뛴 BOM은 세지 않는다)다. 오류 위치는 잘못된 값·문자·이스케이프의 시작이다.
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

// 읽기 한도. 넘으면 해당 오류다. 깊이 한도는 JsonWriter의 기본 깊이 한도와 같다(더 깊게 읽은 문서는
// 같은 maxDepth를 준 writer로 쓴다).
struct JsonReadOptions {
    uint64 maxInputBytes = 64ull << 20;
    uint32 maxDepth = 128;
    uint64 maxStringBytes = 16ull << 20;
    uint32 maxNodes = 4194304;
};

class JsonDocument;

// 문서 안 값 하나를 가리키는 가벼운 핸들이다. 문서가 살아 있는 동안 쓴다. 문서를 다시 Parse하거나
// Clear하면 핸들이 무효가 되고(세대 번호로 판별) IsValid()가 false가 된다. 기본 생성 값과 없는 키의
// Find 결과도 무효다. 무효 값의 Is*는 false, Size()는 0이고 그 밖의 접근자는 Fatal이다.
class JsonValue {
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
    // 원문 숫자 토큰(예: "1.50", "-0"). 전제: IsNumber().
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

private:
    friend class JsonDocument;
    friend bool JsonEquals(JsonValue left, JsonValue right) noexcept;
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
// Canonical 출력 byte가 같다는 뜻은 아니다(1과 1.0). 깊이와 무관하게 재귀하지 않는다. 무효 값끼리는
// 같다.
bool JsonEquals(JsonValue left, JsonValue right) noexcept;

// 읽은 JSON 문서다. 노드·문자열을 allocator의 평평한 배열 몇 개에 담는다. 복사·이동하지 않는다.
class JsonDocument final {
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
    friend bool JsonEquals(JsonValue left, JsonValue right) noexcept;

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

    // 중복 키 판정과 조회용 색인 키: (객체 노드, text_의 키 범위). 조회는 (객체 노드, StringView).
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

// StringBuilder에 JSON을 쓴다. byte 단위 고정 규칙: 들여쓰기 2칸, 줄바꿈 LF, Finish가 끝에 줄바꿈
// 1개, 키 구분자 ": ", 빈 컨테이너 {}·[], 키 순서는 호출 순서. 문자열은 '"'·'\\'·제어 문자만
// 이스케이프한다(U+0008·0C·0A·0D·09는 \b\f\n\r\t, 그 밖 제어 문자는 소문자 \u00xx). 그 밖의
// 문자('/'·U+007F·U+2028 포함)는 UTF-8 그대로이며 입력 문자열의 UTF-8을 검증한다. BOM은 쓰지
// 않는다. 입력 문자열이 out의 View()여도 된다. 들여쓰기 때문에 출력 크기는 중첩 깊이의 제곱에
// 비례한다(깊이 D인 사슬은 약 2·D² byte) — 아주 깊은 문서는 maxDepth를 올려도 출력이 커진다.
//
// 오류: 구조 오류(키 없이 값, 같은 객체의 중복 키 — 자기 reader가 거부한다, 닫지 않은 컨테이너,
// Finish 뒤 호출 등)·NaN·Inf·잘못된 UTF-8·무효 JsonValue는 InvalidArgument, maxDepth 초과는
// CapacityExceeded, builder·색인 할당 실패는 OutOfMemory. 한 번 실패하면 이후 호출은 모두 실패하고
// builder에는 실패 전까지 쓴 내용이 남는다(그 결과는 버린다). 복사하지 않는다.
class JsonWriter final {
public:
    // maxDepth는 JsonReadOptions::maxDepth와 같은 기본값이다(쓴 결과를 기본 옵션으로 다시 읽을 수
    // 있다).
    explicit JsonWriter(StringBuilderBase& out, JsonNumberMode mode = JsonNumberMode::Canonical,
                        uint32 maxDepth = 128) noexcept;
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

    struct WrittenKeyHash {
        const StringBuilderBase* out;
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
    Result<void> WriteEscaped(StringView text) noexcept;
    Result<void> WriteIndent(usize depth) noexcept;
    Result<void> Fail(ErrorCode code) noexcept;
    Result<void> Check(Result<void> result) noexcept;

    StringBuilderBase* out_;
    JsonNumberMode mode_;
    uint32 maxDepth_;
    Array<Frame> frames_;
    HashMap<WrittenKey, uint8, WrittenKeyHash, WrittenKeyEqual> keys_;
    uint32 nextSerial_ = 0;
    bool expectingValue_ = false; // 객체에서 Key 뒤
    bool rootWritten_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace rocks
