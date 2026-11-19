#pragma once

#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/LogStructure.h>
#include <Core/Text/StringBuilder.h>

// 펼친 값("{@Name}", LG16)의 캡처 인코딩이다. LogStructureWriter가 쓰고 렌더(LogCapture.cpp)가
// 읽는다. 같은 프로세스 안에서만 쓰는 형식이라 버전이 없다(파일·wire 형식이 아니다).
//
// 기록 하나: [머리 1 byte][이름][값]
// - 머리: 아래 4 bit가 Record 종류, HasName·Truncated 비트
// - 이름(HasName일 때만, 객체의 필드): 정적 문자열 포인터 + uint16 길이
// - 값: Bool 1 byte, Signed·Unsigned·Float 8 byte(Float는 double bit), String uint32 길이 + byte,
//   Symbol 정적 문자열(포인터 + uint16 길이), Object 타입 태그(정적 문자열), Array uint64 전체 원소
//   수, 그 밖은 없음
// 흐름: 맨 위는 이름 없는 Object 하나이고 End로 닫힌다. Object·Array는 End까지가 내용이다. 깊이는
// MaxLogStructureDepth를 넘지 않는다. 정렬되지 않은 위치라 값은 memcpy로 읽고 쓴다.
namespace rocks::detail::log_structure {

enum class Record : uint8 {
    Null = 1,
    Bool,
    Signed,
    Unsigned,
    Float,
    String,
    Symbol,
    Unsupported,
    FormatError,
    Object,
    Array,
    End
};

inline constexpr uint8 KindMask = 0x0Fu;
inline constexpr uint8 HasName = 1u << 4;
// String: 값이 잘렸다. Object·Array: 상한·공간 때문에 내용 일부를 버렸다.
inline constexpr uint8 Truncated = 1u << 5;

// 기록 머리(종류·HasName·Truncated)의 byte 수다.
inline constexpr usize HeadBytes = 1;
// Bool 값 칸의 byte 수다(0·1).
inline constexpr usize BoolBytes = 1;
// 정적 문자열 칸(포인터 + 길이)의 byte 수다.
inline constexpr usize StaticTextBytes = sizeof(const char*) + sizeof(uint16);
// 원시 값 칸(정수·실수 bit·배열 원소 수)의 byte 수다.
inline constexpr usize ScalarBytes = sizeof(uint64);
// 문자열 길이 칸의 byte 수다.
inline constexpr usize LengthBytes = sizeof(uint32);
// End 기록의 byte 수다(머리뿐). 열린 객체·배열마다 이만큼을 남겨 둔다.
inline constexpr usize EndBytes = HeadBytes;

static_assert(MaxLogStaticNameBytes <= NumericLimits<uint16>::Max(),
              "static name length is stored as uint16");

// 읽은 기록 하나다. view는 인코딩 byte나 정적 문자열을 가리킨다.
// - name: HasName일 때 필드 이름
// - text: String byte·Symbol·Object 태그
// - bits: Bool(0·1)·Signed·Unsigned·Float bit·Array 전체 원소 수
struct Node {
    Record kind = Record::Null;
    bool truncated = false;
    bool hasName = false;
    StringView name;
    StringView text;
    uint64 bits = 0;
};

// 인코딩을 앞에서부터 한 기록씩 읽는다. 범위·종류만 확인한다(흐름은 Validate가 본다).
class Cursor {
public:
    explicit Cursor(Span<const uint8> stream) noexcept : stream_(stream) {
    }

    // 다음 기록을 node에 쓰고 true다. 끝이거나 손상이면 false다(Failed로 구별한다).
    bool Next(Node& node) noexcept;

    bool Failed() const noexcept {
        return failed_;
    }

private:
    bool Read(void* out, usize bytes) noexcept;
    bool ReadStatic(StringView& text) noexcept;

    Span<const uint8> stream_;
    usize position_ = 0;
    bool failed_ = false;
};

// 인코딩 전체가 올바른지다: 기록 범위·종류, 맨 위 이름 없는 Object 하나와 그 End, 객체 필드에만
// 이름, 깊이 상한, End 짝. 빈 인코딩(공간이 없어 아무것도 담지 못함)도 올바르다.
bool Validate(Span<const uint8> stream) noexcept;

// 텍스트 모양(`Tag { Name: "Ann", Hp: 30 }`)으로 out에 붙인다. 빈 인코딩은 "{ ... }"다. 반환은 서식
// 실패(잘못된 UTF-8 문자열·Formatter 오류 값)가 있었는지다. Validate를 통과한 인코딩만 넘긴다.
bool AppendText(FixedStringBuilder& out, Span<const uint8> stream) noexcept;

// 노드 채우기 상태다(기록 한 건의 모든 펼친 값이 nodes를 함께 쓴다).
// - nodes·used: 노드 칸과 쓴 수
// - texts: 숫자 노드의 기본 서식 텍스트를 쓰는 곳(속성 텍스트 영역)
// - overflow: 노드 칸이 모자라 버렸다
// - formatError: 잘못된 UTF-8 문자열·Formatter 오류 값이 있었다
struct NodeFill {
    Span<LogProperty> nodes;
    usize used = 0;
    FixedStringBuilder* texts = nullptr;
    bool overflow = false;
    bool formatError = false;
};

// 맨 위 속성 top(kind·typeTag·truncated·subtreeBegin·subtreeCount)과 그 자손 노드를 채운다. top의
// name·text는 호출자가 채운다. Validate를 통과한 인코딩만 넘긴다.
void FillNodes(Span<const uint8> stream, LogProperty& top, NodeFill& fill) noexcept;

} // namespace rocks::detail::log_structure
