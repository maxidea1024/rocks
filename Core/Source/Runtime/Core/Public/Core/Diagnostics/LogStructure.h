#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 로그 구조체 펼치기(Serilog `{@Obj}`)의 확장점이다(Docs/Design/LoggingSystem.ko.md 14.1.6절, L19).
//
// 메시지 템플릿의 "{@Name}" 자리에 넣은 인자는 Formatter로 한 줄을 만들지 않고, 필드 이름과 값을
// 캡처 시점에 기록으로 복사한다. 렌더는 텍스트로 `Player { Name: "Ann", Hp: 30 }`, JSON lines로
// props 안 중첩 객체를 낸다.
//
// 타입은 LogStructure<T>를 특수화해 필드를 알린다. 리플렉션 타입(ROCKS_STRUCT·ROCKS_CLASS)은
// ObjectRuntime의 <Object/LogStructure.h>가 생성 필드 방문 함수로 일반 특수화를 준다. Core는 이
// 확장점만 정의한다(Core는 리플렉션 모듈 아래다).
//
// 사용 예(손으로 쓴 특수화):
//   struct Player { String name; int32 hp; Array<int32> scores; };
//   template <> struct rocks::LogStructure<Player> {
//       static StringView TypeTag() noexcept { return "Player"; } // 없어도 된다(태그 없음)
//       static void Write(LogStructureWriter& out, const Player& value) noexcept {
//           out.Write("Name", value.name);
//           out.Write("Hp", value.hp);
//           out.Write("Scores", value.scores); // 앞 MaxLogStructureElements개와 전체 개수
//       }
//   };
//   Log::Info(LogGame, "Spawned {@Player}", player);
//
// 상한: 깊이 MaxLogStructureDepth, 객체마다 필드 MaxLogStructureFields, 배열은 앞
// MaxLogStructureElements개와 전체 개수. 넘치면 그 객체·배열에 잘림 표시가 남고 기록에
// LogEventFlags::ValueTruncated가 선다. 크기는 캡처 슬롯·블록 한도(설계 8.3절)를 따른다.
namespace rocks {

// 펼친 값의 최대 중첩 깊이다. 맨 위 객체가 1이고 객체·배열마다 1씩 늘어난다. 더 깊은 객체·배열은
// 내용 없이 잘림 표시만 남는다.
inline constexpr usize MaxLogStructureDepth = 4;

// 객체 하나에 담는 최대 필드 수다. 넘는 필드는 버리고 그 객체에 잘림 표시를 남긴다.
inline constexpr usize MaxLogStructureFields = 32;

// 배열 하나에 담는 최대 원소 수다. 전체 원소 수는 따로 담는다(넘으면 잘림 표시).
inline constexpr usize MaxLogStructureElements = 8;

// 기록 한 건을 렌더할 때 펼친 값의 노드(LogEvent::structureNodes)를 담는 최대 수다(맨 위 속성
// 제외). 넘으면 노드를 더 만들지 않고 LogEventFlags::MessageTruncated가 선다(메시지 텍스트는
// 노드와 무관하게 다 렌더된다).
inline constexpr usize MaxLogStructureNodes = 128;

// 이름·태그·기호 하나의 최대 byte 수다(캡처가 길이를 16-bit로 담는다). 넘으면 UTF-8 문자 경계에서
// 자른다.
inline constexpr usize MaxLogStaticNameBytes = NumericLimits<uint16>::Max();

static_assert(MaxLogStructureElements <= MaxLogStructureNodes &&
                  MaxLogStructureFields <= MaxLogStructureNodes,
              "one full object or array must fit in the render node table");

// 정적 저장소 문자열(필드 이름·타입 태그·enum 열거자 이름)이다. 캡처는 이 포인터만 담고 렌더는
// 출력 스레드가 나중에 하므로, 문자열은 프로그램(또는 그 문자열을 담은 모듈) 수명 동안 살아 있어야
// 한다.
//
// - 문자열 리터럴은 consteval 생성자로 바로 바뀐다: out.Write("Hp", value.hp)
// - 생성 코드 표처럼 리터럴에서 온 view는 FromStatic으로 만든다(정적 저장소라는 것은 호출자가
//   보장한다).
// - 기본값은 이름 없음이다(배열 원소·맨 위 객체).
class LogStaticName {
public:
    constexpr LogStaticName() noexcept = default;

    template <usize Count>
    consteval LogStaticName(const char (&text)[Count]) noexcept : text_(text, Count - 1) {
    }

    // 정적 저장소 문자열의 view에서 만든다. text가 가리키는 byte는 로그 기록이 렌더될 때까지(보통
    // 프로그램 끝까지) 바뀌거나 해제되면 안 된다.
    static constexpr LogStaticName FromStatic(StringView text) noexcept {
        LogStaticName name;
        name.text_ = text;
        return name;
    }

    constexpr StringView View() const noexcept {
        return text_;
    }

private:
    StringView text_;
};

class LogStructureWriter;

// 구조체 펼치기 확장점이다. 특수화는 다음을 가진다.
// - static void Write(LogStructureWriter& out, const T& value) noexcept: 필드를 out에 쓴다. 호출
//   스레드에서 캡처 중에 한 번 불린다(기록이 꺼져 있으면 부르지 않는다). 값 읽기만 하고 할당·잠금·
//   로그 호출을 하지 않아야 한다(생산자 경로, 설계 8.1절).
// - (선택) static StringView TypeTag() noexcept: 텍스트 `Tag { ... }`와 JSON "$type"의 타입
//   태그다. 정적 저장소 문자열이어야 한다. 없으면 태그가 없다.
// "{@Name}" 자리 인자의 타입에 특수화가 없으면 템플릿이 컴파일되지 않는다. T는 클래스 타입이어야
// 한다(enum·정수에 특수화하면 쓰는 곳에서 static_assert로 거부한다).
template <class T>
struct LogStructure;

// T에 LogStructure 특수화(Write)가 있는지다.
template <class T>
concept HasLogStructure = requires(LogStructureWriter& out, const T& value) {
    { LogStructure<T>::Write(out, value) } noexcept;
};

namespace detail {

// LogStructure 특수화는 클래스 타입에만 둔다. 캡처는 클래스 인자의 객체 주소로 펼치고, 정수·enum은
// 원시 값으로 캡처하므로 "{@Name}"에 쓸 수 없다(템플릿 검증과 런타임이 어긋나지 않게 컴파일 때
// 막는다).
template <class U>
consteval void CheckLogStructureType() noexcept {
    static_assert(IsClassV<U>,
                  "rocks::LogStructure<T> is only supported for class types; log enums and "
                  "scalars with {Name} holes instead");
}

template <class T>
concept HasLogStructureTypeTag = requires {
    { LogStructure<T>::TypeTag() } noexcept;
    requires IsConvertibleV<decltype(LogStructure<T>::TypeTag()), StringView>;
};

// 연속 원소 범위(Span·Array처럼 Data()·Size()를 가진 타입)다. 문자열 타입은 앞에서 따로 고른다.
template <class T>
concept LogContiguousRange = requires(const T& value) {
    value.Data();
    requires IsConvertibleV<decltype(value.Size()), usize>;
};

template <class T>
StringView LogStructureTypeTag() noexcept {
    if constexpr (HasLogStructureTypeTag<T>) {
        return LogStructure<T>::TypeTag();
    } else {
        return {};
    }
}

} // namespace detail

// 펼친 값을 캡처 버퍼에 바로 인코딩하는 쓰기 도구다. LogStructure<T>::Write가 받는다.
//
// 쓰는 것: 필드(이름 + 값), 객체 시작·끝, 배열 시작·끝. 객체 안에서는 이름이 필드 이름이고, 배열
// 안에서는 이름을 무시한다(원소). 값은 그 자리에서 복사한다: 정수·실수·bool은 원시 값, 문자열은
// byte, Formatter 타입은 렌더한 문자열, 이름·태그·enum 이름은 정적 포인터.
//
// 실패는 호출자에게 알리지 않는다. 공간·상한이 모자라면 그 값을 버리고 잘림 표시를 남긴다(장애
// 격리, 설계 10절). BeginObject·BeginArray가 false를 돌려주면 그 안의 쓰기는 모두 버려지므로
// 내용을 만들지 않아도 된다. 그래도 짝이 되는 EndObject·EndArray는 부른다.
//
// 스레드: 한 캡처 동안 한 스레드만 쓴다. 힙 할당은 없다.
// 수명: 캡처가 만든다. 시험은 고정 버퍼로 직접 만들어 인코딩만 확인할 수 있다.
class LogStructureWriter {
public:
    // buffer에 인코딩한다. buffer는 쓰기가 끝날 때(Finish)까지 살아 있어야 한다.
    explicit LogStructureWriter(Span<uint8> buffer) noexcept;

    LogStructureWriter(const LogStructureWriter&) = delete;
    LogStructureWriter& operator=(const LogStructureWriter&) = delete;

    void WriteNull(LogStaticName name) noexcept;
    void WriteBool(LogStaticName name, bool value) noexcept;
    void WriteSigned(LogStaticName name, int64 value) noexcept;
    void WriteUnsigned(LogStaticName name, uint64 value) noexcept;
    void WriteFloat(LogStaticName name, double value) noexcept;

    // 문자열 byte를 복사한다(UTF-8 검증은 렌더 때, 잘못되면 그 값이 "<format error>"다). 공간이
    // 모자라면 UTF-8 문자 경계에서 자르고 잘림 표시를 남긴다.
    void WriteString(LogStaticName name, StringView value) noexcept;

    // 정적 저장소의 기호(enum 열거자 이름 등)다. 텍스트는 따옴표 없이, JSON은 문자열로 낸다.
    void WriteSymbol(LogStaticName name, LogStaticName symbol) noexcept;

    // 펼칠 수 없는 값이다. 텍스트·JSON 모두 "<unsupported>"다.
    void WriteUnsupported(LogStaticName name) noexcept;

    // Formatter<T>로 지금 렌더한 문자열을 담는다(spec 없음). Formatter가 실패하면 그 값이
    // "<format error>"다.
    template <class T>
    void WriteFormatted(LogStaticName name, const T& value) noexcept {
        static_assert(detail::HasFormatter<T>,
                      "WriteFormatted requires a rocks::Formatter<T> specialization");
        WriteFormattedErased(name, &value,
                             [](StringBuilderBase& out, const void* object,
                                StringView spec) noexcept -> Result<void> {
                                 return Formatter<T>::Format(out, *static_cast<const T*>(object),
                                                             spec);
                             });
    }

    // 객체를 연다. tag는 타입 태그(없으면 빈 값)다. false면 내용을 버린다(상한·공간).
    bool BeginObject(LogStaticName name, LogStaticName tag = {}) noexcept;
    void EndObject() noexcept;

    // 배열을 연다. totalCount는 전체 원소 수다(담는 원소는 앞 MaxLogStructureElements개). false면
    // 원소를 버린다.
    bool BeginArray(LogStaticName name, usize totalCount) noexcept;
    void EndArray() noexcept;

    // 값 하나를 타입에 맞게 쓴다. 고르는 순서:
    // bool → char(한 글자 문자열) → 정수 → 실수 → 문자열(StringView·String·C 문자열·char 배열,
    // null C 문자열은 null) → LogStructure 특수화(중첩 객체) → Formatter(렌더 문자열) → enum(바탕
    // 정수) → 연속 범위·C 배열(배열) → 그 밖은 "<unsupported>".
    template <class T>
    void Write(LogStaticName name, const T& value) noexcept {
        using U = RemoveCvRefT<T>;
        if constexpr (IsSameV<U, bool>) {
            WriteBool(name, value);
        } else if constexpr (IsSameV<U, char>) {
            WriteString(name, StringView(&value, 1));
        } else if constexpr (IsIntegralV<U>) {
            static_assert(sizeof(U) <= sizeof(uint64),
                          "integers wider than 64 bits are not loggable");
            if constexpr (IsSignedV<U>) {
                WriteSigned(name, static_cast<int64>(value));
            } else {
                WriteUnsigned(name, static_cast<uint64>(value));
            }
        } else if constexpr (IsFloatingPointV<U>) {
            WriteFloat(name, static_cast<double>(value));
        } else if constexpr (IsSameV<U, StringView>) {
            WriteString(name, value);
        } else if constexpr (IsSameV<U, String>) {
            WriteString(name, value.View());
        } else if constexpr (detail::IsCharacterTextPointer<U>) {
            if (value == nullptr) {
                WriteNull(name);
            } else {
                WriteString(name, StringView(value));
            }
        } else if constexpr (detail::IsCharacterArray<U>) {
            WriteCharacterArray(name, value, sizeof(U));
        } else if constexpr (HasLogStructure<U>) {
            detail::CheckLogStructureType<U>();
            if (BeginObject(name, LogStaticName::FromStatic(detail::LogStructureTypeTag<U>()))) {
                LogStructure<U>::Write(*this, value);
            }

            EndObject();
        } else if constexpr (detail::HasFormatter<U>) {
            WriteFormatted(name, value);
        } else if constexpr (IsEnumV<U>) {
            Write(name, static_cast<UnderlyingTypeT<U>>(value));
        } else if constexpr (IsArrayV<U>) {
            WriteElements(name, value, sizeof(U) / sizeof(value[0]));
        } else if constexpr (detail::LogContiguousRange<U>) {
            WriteElements(name, value.Data(), static_cast<usize>(value.Size()));
        } else {
            WriteUnsupported(name);
        }
    }

    // 연속 원소 count개를 배열로 쓴다(앞 MaxLogStructureElements개만 담는다).
    template <class E>
    void WriteElements(LogStaticName name, const E* elements, usize count) noexcept {
        if (BeginArray(name, count)) {
            const usize kept = count < MaxLogStructureElements ? count : MaxLogStructureElements;
            for (usize index = 0; index < kept; ++index) {
                Write(LogStaticName{}, elements[index]);
            }
        }

        EndArray();
    }

    // 열린 객체·배열을 모두 닫고 쓰기를 끝낸다. 그 뒤의 쓰기는 무시된다. 캡처가 부른다.
    void Finish() noexcept;

    // 인코딩한 byte 수다.
    usize Bytes() const noexcept {
        return used_;
    }

    // 상한·공간 때문에 무언가를 버렸거나 잘랐는지다.
    bool Truncated() const noexcept {
        return truncated_;
    }

    // 공간이 모자라 무언가를 버렸거나 잘랐는지다(상한 잘림은 포함하지 않는다). 더 큰 영역에 다시
    // 담으면 달라질 수 있는지를 캡처가 판단하는 데 쓴다.
    bool SpaceShort() const noexcept {
        return spaceShort_;
    }

private:
    struct Frame {
        usize offset = 0;
        usize children = 0;
        bool array = false;
    };

    void WriteFormattedErased(LogStaticName name, const void* object,
                              detail::FormatArg::CustomFunction format) noexcept;
    void WriteCharacterArray(LogStaticName name, const char* value, usize capacity) noexcept;
    // 자식 하나를 더할 자리인지 판정하고 부모의 자식 수를 센다(상한을 넘으면 부모에 잘림 표시).
    bool BeginChild() noexcept;
    // 머리(종류·이름)를 쓴다. 머리와 payloadBytes, 열린 객체·배열을 닫을 byte가 들어가지 않으면
    // 공간 고갈로 표시하고 false다.
    bool PutHeader(uint8 kind, LogStaticName name, usize payloadBytes) noexcept;
    void PutBytes(const void* data, usize bytes) noexcept;
    void PutStatic(StringView text) noexcept;
    bool BeginContainer(uint8 kind, LogStaticName name, LogStaticName tag,
                        usize totalCount) noexcept;
    void EndContainer() noexcept;
    void MarkFrameTruncated(const Frame& frame) noexcept;
    // 공간이 모자라 값을 버리거나 잘랐다: 열린 객체·배열 모두에 잘림 표시를 남긴다.
    void MarkSpaceShort() noexcept;
    void Exhaust() noexcept;
    bool InArray() const noexcept;
    usize Available() const noexcept;

    Span<uint8> buffer_;
    usize used_ = 0;
    usize depth_ = 0;
    // 버리는 중인 객체·배열의 중첩 수다(0이 아니면 그 안의 쓰기는 모두 무시한다).
    usize skip_ = 0;
    Frame frames_[MaxLogStructureDepth];
    bool truncated_ = false;
    bool spaceShort_ = false;
    bool exhausted_ = false;
    bool finished_ = false;
};

namespace detail {

// 타입을 지운 "맨 위 값 쓰기" 함수다. object는 T 객체다. 캡처가 "{@Name}" 자리 인자마다 부른다.
using LogStructureWrite = void (*)(LogStructureWriter& out, const void* object) noexcept;

template <class T>
void WriteLogStructureRoot(LogStructureWriter& out, const void* object) noexcept {
    out.Write(LogStaticName{}, *static_cast<const T*>(object));
}

// 로그 인자 타입의 맨 위 값 쓰기 함수다. LogStructure 특수화가 없으면 nullptr다.
template <class T>
consteval LogStructureWrite LogStructureWriteOf() noexcept {
    using U = RemoveCvRefT<T>;
    if constexpr (HasLogStructure<U>) {
        CheckLogStructureType<U>();
        return &WriteLogStructureRoot<U>;
    } else {
        return nullptr;
    }
}

// Formatter 없이 LogStructure만 가진 클래스 타입이다. "{@Name}" 자리에만 쓸 수 있다.
template <class U>
inline constexpr bool IsLogStructureOnly = IsClassV<U> && !HasFormatter<U> && HasLogStructure<U>;

// Formatter도 LogStructure도 없는 클래스 타입이다(로그 인자로 쓸 수 없다).
template <class U>
inline constexpr bool IsUnloggableClass = IsClassV<U> && !HasFormatter<U> && !HasLogStructure<U> &&
                                          !IsSameV<U, StringView> && !IsSameV<U, String>;

} // namespace detail

} // namespace rocks
