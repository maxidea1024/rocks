#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Config/ConsoleSettings.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// CVar 핸들의 상수 정의 부분이다(Docs/Design/CVarSystem.ko.md 4·5·10.1절 — CVR1). 핸들(`CVar<T>`·
// `BootCVar<T>` — Config/CVar.h)은 이 정의를 상수 초기화로 품고, ConfigContext::Register는 타입을
// 지운 정의(detail::VarDefinition)를 받는다. 사용자는 Config/CVar.h만 include한다.
//
// 스레드: 정의의 상수 부분은 컴파일 때 정해져 어느 스레드가 읽어도 경쟁이 없다. 바뀌는 칸은 번호
// (VarDefinition::index) 하나이고 첫 Register가 원자적 CAS로 한 번만 쓴다(10.1절 표).
namespace rocks::cvar {

// 핸들 번호다(5절). 처음 등록될 때 프로세스 전체에서 한 번 정해지고 바뀌지 않는다. 문맥이
// 여럿이어도 같은 핸들은 같은 번호다. 번호는 실행 중에만 쓰고 출력·wire에는 이름을 쓴다.
//
// 사용 예:
//   if (ShadowDistance.Index().IsValid()) { ... }   // 등록된 적이 있다
struct Index {
    // 번호 없음(아직 등록되지 않음) 표지다.
    static constexpr uint32 None = NumericLimits<uint32>::Max();

    uint32 value = None;

    constexpr bool IsValid() const noexcept {
        return value != None;
    }

    friend constexpr bool operator==(Index left, Index right) noexcept = default;
};

// 핸들 종류다. 정의의 공통 머리(VarDefinition)에 있어 ConfigContext::Register가 타입을 지운 정의를
// 종류대로 다룬다(Group이면 GroupDefinition — Config/CVarGroup.h, Command면 CommandDefinition —
// Config/CVarCommand.h).
enum class HandleKind : uint8 {
    // 실행 중 바꿀 수 있는 변수(`CVar<T>`).
    Var,
    // 이번 실행 동안 고정인 변수(`BootCVar<T>` — 9절).
    Boot,
    // 프리셋 묶음(`CVarGroup` — 11절). 값은 int32 등급이고 정의는 GroupDefinition이다.
    Group,
    // 콘솔 명령(`ConsoleCommand` — 13·15절). 값이 없고 정의는 CommandDefinition이다.
    Command
};

// 빌더가 정의에 남기는 성질 비트다(4.2절).
enum class VarFlags : uint32 {
    None = 0,
    // 출시 빌드에서 등록되지 않고 `Get()`이 기본값이다(13.1절, Config DevelopmentOnly를 대신한다 —
    // VD5).
    Cheat = 1,
    // 서버가 복제하는 값이다(13.2절 — 네트워크 역할 검사는 CVR6).
    Replicated = 2,
    // 사용자 층에 쓸 수 있다.
    UserSetting = 4,
    // 콘솔 층만 거부한다.
    ConsoleReadOnly = 8,
    // `Range`가 있다.
    HasRange = 16,
    // 범위 밖 값을 범위 끝으로 보정한다(`Clamp` — `HasRange`가 있어야 한다).
    Clamp = 32
};

constexpr bool HasFlag(uint32 flags, VarFlags flag) noexcept {
    return (flags & static_cast<uint32>(flag)) != 0;
}

// 값을 직접 정한 enum의 이름 표 항목이다(4.4절 — `CVarEnumNames`가 `Span<const EnumEntry>`를 돌려줄
// 때).
struct EnumEntry {
    StringView name;
    int64 value = 0;
};

// 수 CVar 타입(int32·int64·float·double)인지다. `Range`·`Clamp`는 이 타입만 받는다.
template <class T>
inline constexpr bool IsNumericVarType =
    IsSameV<T, int32> || IsSameV<T, int64> || IsSameV<T, float> || IsSameV<T, double>;

// 문자열 CVar 타입(String·Name)인지다. 기본값과 검사 함수 인자는 StringView다.
template <class T>
inline constexpr bool IsTextVarType = IsSameV<T, String> || IsSameV<T, rocks::Name>;

// CVar가 받는 타입인지다(4.1절 표): bool·int32·int64·float·double·String·Name·enum class.
template <class T>
inline constexpr bool IsVarType =
    IsSameV<T, bool> || IsNumericVarType<T> || IsTextVarType<T> || IsScopedEnumV<T>;

// `Validate` 검사 함수의 인자 타입이다(4.2절): String·Name이면 StringView, 그 밖에는 const T&.
template <class T>
using ValidateArg = ConditionalT<IsTextVarType<T>, StringView, const T&>;

// `Validate` 검사 함수다. 통과면 빈 view, 위반이면 이유(영어 리터럴 — 정적 저장소)를 돌려준다. 입력
// 값만 보는 순수 함수여야 하고 상수 평가가 가능해야 한다(기본값을 컴파일 때 이 함수로 검사한다).
template <class T>
using ValidateFunction = StringView (*)(ValidateArg<T>) noexcept;

// `DefaultValue()`의 반환 타입이다: String·Name이면 StringView(정적 저장소의 기본값 텍스트), 그
// 밖은 T.
template <class T>
using DefaultArg = ConditionalT<IsTextVarType<T>, StringView, T>;

// 정의 텍스트의 byte 한도다(컴파일 때 검사 — 4.3절). 콘솔 이름은 ASCII 소문자 점 경로다.
inline constexpr usize MaxVarNameBytes = 128;
inline constexpr usize MaxVarHelpBytes = 1024;
inline constexpr usize MaxVarCategoryBytes = 64;
inline constexpr usize MaxVarDefaultTextBytes = 4096;

// Name 기본값의 byte 한도다. Name 기본값은 출력 가능한 ASCII(0x20–0x7E)로 한정하므로 byte 수가 곧
// UTF-16 단위 수이고 NFC 판정이 필요 없다(Name 내용 규칙 — Object/NameField.h
// MaxNameBaseUtf16Units 1,023과 같은 값).
inline constexpr usize MaxVarNameDefaultBytes = 1023;

namespace detail {

// text가 NUL 없는 유효 UTF-8인지다(컴파일 때 정의 텍스트 검사).
constexpr bool IsDefinitionText(StringView text) noexcept {
    for (const char c : text) {
        if (c == '\0') {
            return false;
        }
    }

    return text.IsValidUtf8();
}

// text가 출력 가능한 ASCII(0x20–0x7E)로만 되어 있는지다(Name 기본값).
constexpr bool IsPrintableAscii(StringView text) noexcept {
    for (const char c : text) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }

    return true;
}

// 컴파일 때 검사의 표지 함수다. constexpr이 아니므로 consteval 변환 안에서 부르면 컴파일 오류가
// 나고 진단에 함수 이름이 나온다(LogCategory와 같은 방식 — 4.3절).
inline void CVarError_InvalidName() noexcept {
}

inline void CVarError_MissingHelp() noexcept {
}

inline void CVarError_RangeMinimumAboveMaximum() noexcept {
}

inline void CVarError_ClampWithoutRange() noexcept {
}

inline void CVarError_DefaultOutOfRange() noexcept {
}

inline void CVarError_DefaultFailsValidate() noexcept {
}

inline void CVarError_DefaultNotInEnum() noexcept {
}

inline void CVarError_InvalidEnumNames() noexcept {
}

inline void CVarError_TextTooLong() noexcept {
}

inline void CVarError_InvalidUtf8() noexcept {
}

inline void CVarError_NameDefaultNotAscii() noexcept {
}

// 프리셋 묶음(11절)의 표지 함수다.
inline void CVarError_DuplicateLevel() noexcept {
}

inline void CVarError_DuplicateEntry() noexcept {
}

inline void CVarError_DefaultLevelUndefined() noexcept {
}

inline void CVarError_TooManyLevels() noexcept {
}

// 타입을 지운 값 하나다(검사 함수가 읽고 보정한다). bool은 boolean, 정수·enum은 integer, 실수는
// real(float도 double로), String·Name은 text다. text는 빌린 view다.
struct ScalarValue {
    bool boolean = false;
    int64 integer = 0;
    double real = 0;
    StringView text{};
};

// 값 검사 결과다.
enum class ValueCheck : uint8 {
    // 그대로 받는다.
    Accepted,
    // 범위 끝으로 보정해 받는다(`Clamp`).
    Clamped,
    // 범위 밖이라 거부한다.
    OutOfRange,
    // `Validate`가 거부했다.
    ValidationFailed,
    // 프리셋 묶음이 정의하지 않은 등급이다(11절 — UndefinedLevel).
    UndefinedLevel
};

struct VarDefinition;

// 범위·보정·`Validate`를 타입대로 적용하는 검사 함수다(CheckValue<T>). reason에 거부 이유를 쓴다.
using CheckValueFunction = ValueCheck (*)(const VarDefinition& definition, ScalarValue& value,
                                          StringView& reason) noexcept;

// enum 값의 열거자 스키마를 돌려주는 함수다(리플렉션 enum은 생성 메타데이터, 그 밖은
// `CVarEnumNames` 표로 만든 상수 스키마 — Config/CVarEnum.h).
using EnumSchemaFunction = const EnumSchema& (*)() noexcept;

// 핸들 하나의 상수 정의다(타입을 지운 부분). 빌더의 consteval 변환이 채우고, 핸들 안에서 상수
// 초기화된다. 이름·도움말·분류·기본값 텍스트는 문자열 리터럴(정적 저장소)을 가리킨다.
struct VarDefinition {
    // 콘솔 이름(소문자 점 경로 — Config D7, VD2)이다. 출시 빌드의 `Cheat()` 정의는 비어
    // 있다(13.1절).
    StringView name;
    StringView help;
    // cvar.find 분류(비어 있을 수 있다).
    StringView category;
    config::ConsoleValueType type = config::ConsoleValueType::Bool;
    HandleKind kind = HandleKind::Var;
    // VarFlags 비트.
    uint32 flags = 0;
    ScalarValue defaultValue;
    // 수 타입의 닫힌 범위다(HasRange일 때).
    ScalarValue minimum;
    ScalarValue maximum;
    // enum 타입만.
    EnumSchemaFunction enumSchema = nullptr;
    CheckValueFunction check = nullptr;
    // 프로세스 전체 번호(Index::None이면 아직 등록되지 않았다). 첫 Register가 CAS로 한 번 쓰고, 그
    // 뒤로는 읽기만 한다(relaxed — 10.1절).
    mutable uint32 index = Index::None;
};

// 타입이 있는 정의다(검사 함수 포인터는 T에 따라 모양이 다르므로 여기에 둔다).
template <class T>
struct TypedDefinition : VarDefinition {
    ValidateFunction<T> validate = nullptr;
};

template <class T>
constexpr config::ConsoleValueType ValueTypeOf() noexcept {
    if constexpr (IsSameV<T, bool>) {
        return config::ConsoleValueType::Bool;
    } else if constexpr (IsSameV<T, int32>) {
        return config::ConsoleValueType::Int32;
    } else if constexpr (IsSameV<T, int64>) {
        return config::ConsoleValueType::Int64;
    } else if constexpr (IsSameV<T, float>) {
        return config::ConsoleValueType::Float;
    } else if constexpr (IsSameV<T, double>) {
        return config::ConsoleValueType::Double;
    } else if constexpr (IsSameV<T, String>) {
        return config::ConsoleValueType::String;
    } else if constexpr (IsSameV<T, rocks::Name>) {
        return config::ConsoleValueType::Name;
    } else {
        return config::ConsoleValueType::Enum;
    }
}

// 텍스트가 아닌 T 값의 ScalarValue다.
template <class T>
constexpr ScalarValue ScalarOf(T value) noexcept {
    ScalarValue scalar;
    if constexpr (IsSameV<T, bool>) {
        scalar.boolean = value;
    } else if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
        scalar.real = static_cast<double>(value);
    } else {
        scalar.integer = static_cast<int64>(value);
    }

    return scalar;
}

// ScalarValue에서 T 값을 되돌린다(텍스트 타입은 StringView).
template <class T>
constexpr DefaultArg<T> ValueOf(const ScalarValue& scalar) noexcept {
    if constexpr (IsTextVarType<T>) {
        return scalar.text;
    } else if constexpr (IsSameV<T, bool>) {
        return scalar.boolean;
    } else if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
        return static_cast<T>(scalar.real);
    } else {
        return static_cast<T>(scalar.integer);
    }
}

// 수 값이 [minimum, maximum] 안인지 보고, 밖이면 Clamp 정의는 끝으로 보정한다. NaN은 보정하지 않고
// 거부한다.
template <class T>
constexpr ValueCheck CheckRange(const VarDefinition& definition, ScalarValue& value) noexcept {
    if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
        if (value.real >= definition.minimum.real && value.real <= definition.maximum.real) {
            return ValueCheck::Accepted;
        }

        if (value.real != value.real || !HasFlag(definition.flags, VarFlags::Clamp)) {
            return ValueCheck::OutOfRange;
        }

        value.real = value.real < definition.minimum.real ? definition.minimum.real
                                                          : definition.maximum.real;
    } else {
        if (value.integer >= definition.minimum.integer &&
            value.integer <= definition.maximum.integer) {
            return ValueCheck::Accepted;
        }

        if (!HasFlag(definition.flags, VarFlags::Clamp)) {
            return ValueCheck::OutOfRange;
        }

        value.integer = value.integer < definition.minimum.integer ? definition.minimum.integer
                                                                   : definition.maximum.integer;
    }

    return ValueCheck::Clamped;
}

// 정의(TypedDefinition<T>)의 범위·보정·`Validate`를 value에 적용한다. 컴파일 때 기본값 검사와 실행
// 중 값 검사가 같은 함수를 쓴다. 전제: definition은 TypedDefinition<T>다.
template <class T>
constexpr ValueCheck CheckValue(const VarDefinition& definition, ScalarValue& value,
                                StringView& reason) noexcept {
    ValueCheck result = ValueCheck::Accepted;
    if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
        // NaN·무한대는 늘 거부한다(범위·Clamp와 무관 — 4.2절). 상수 평가에서 NaN을 만들지 않도록
        // 무한대는 비교로 가린다.
        constexpr double Largest = NumericLimits<double>::Max();
        if (value.real != value.real || value.real > Largest || value.real < -Largest) {
            reason = "the value is not a finite number";
            return ValueCheck::OutOfRange;
        }
    }

    if constexpr (IsNumericVarType<T>) {
        if (HasFlag(definition.flags, VarFlags::HasRange)) {
            result = CheckRange<T>(definition, value);
            if (result == ValueCheck::OutOfRange) {
                reason = "the value is outside the setting range";
                return result;
            }
        }
    }

    const auto& typed = static_cast<const TypedDefinition<T>&>(definition);
    if (typed.validate != nullptr) {
        if constexpr (IsTextVarType<T>) {
            reason = typed.validate(value.text);
        } else {
            const T argument = ValueOf<T>(value);
            reason = typed.validate(argument);
        }

        if (!reason.IsEmpty()) {
            return ValueCheck::ValidationFailed;
        }
    }

    return result;
}

// 핸들의 정의에 닿는 길이다(ConfigContext::Register가 쓴다). 핸들은 이 struct를 friend로 둔다.
struct HandleAccess {
    template <class Handle>
    static constexpr const VarDefinition& Definition(const Handle& handle) noexcept {
        return handle.definition_;
    }
};

// ConfigContext::Register가 받는 핸들 타입인지다(Config/CVar.h가 핸들마다 특수화한다).
template <class Handle>
inline constexpr bool IsHandle = false;

// 템플릿 인자에 의존하는 거짓이다(쓰는 순간에만 실패하는 static_assert용).
template <class... Types>
inline constexpr bool DependentFalse = false;

} // namespace detail

} // namespace rocks::cvar
