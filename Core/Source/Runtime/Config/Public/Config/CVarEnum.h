#pragma once

#include <Config/CVarDefinition.h>
#include <Core/Containers/Span.h>
#include <Meta/GeneratedType.h>

// enum CVar의 이름 표다(Docs/Design/CVarSystem.ko.md 4.4절, VD11). 찾는 순서는 리플렉션 enum
// (ROCKS_ENUM — 생성 메타데이터의 열거자 이름·도구 이름) → 같은 namespace의 constexpr
// `CVarEnumNames(E)`(ADL)다. 둘 다 없으면 static_assert다.
//
// `CVarEnumNames(E)`는 둘 중 하나를 돌려준다:
// - `Span<const StringView>`: 값이 0부터 차례인 enum(이름 i의 값이 i).
// - `Span<const cvar::EnumEntry>`: 값을 직접 정한 enum(이름·값 쌍).
// enum에 `Count` 열거자가 있으면 표 크기가 `Count`와 같아야 한다(`Count`는 값이 아니다).
//
// 사용 예:
//   enum class DebugView : int32 { None, Normals, Depth, Count };
//
//   constexpr Span<const StringView> CVarEnumNames(DebugView) noexcept {
//       static constexpr StringView names[] = {"None", "Normals", "Depth"};
//       return names;
//   }
//
// 이름 입력은 ASCII 대소문자를 무시한다(리플렉션 enum과 같은 열거자 조회 규칙 — Meta
// FindEnumerator). 수 입력과 `3 (High)` 표시는 콘솔 도구 단계(CVR6)다.
namespace rocks::cvar::detail {

// 리플렉션 enum(생성 헤더를 include한 ROCKS_ENUM)인지다.
template <class E>
concept ReflectedEnum = IsEnumV<E> && requires(int64 value) {
    GeneratedType<E>::Schema();
    GeneratedType<E>::IsDeclared(value);
};

// ADL로 찾는 `CVarEnumNames(E)`가 있는지다.
template <class E>
concept NamedEnum = IsEnumV<E> && requires(E value) { CVarEnumNames(value); };

// enum CVar의 이름을 찾을 수 있는지다.
template <class E>
inline constexpr bool HasEnumNames = ReflectedEnum<E> || NamedEnum<E>;

// ADL 이름 표다(리플렉션 enum이 아닐 때만 쓴다).
template <class E>
constexpr auto EnumTable() noexcept {
    return CVarEnumNames(E{});
}

template <class E>
inline constexpr bool IsNameTable = IsSameV<decltype(EnumTable<E>()), Span<const StringView>>;

template <class E>
inline constexpr bool IsEntryTable = IsSameV<decltype(EnumTable<E>()), Span<const EnumEntry>>;

template <class E>
constexpr StringView EnumTableName(usize index) noexcept {
    if constexpr (IsNameTable<E>) {
        return EnumTable<E>()[index];
    } else {
        return EnumTable<E>()[index].name;
    }
}

template <class E>
constexpr int64 EnumTableValue(usize index) noexcept {
    if constexpr (IsNameTable<E>) {
        return static_cast<int64>(index);
    } else {
        return EnumTable<E>()[index].value;
    }
}

// ADL 표 모양을 컴파일 때 확인한다(반환 타입, `Count`와 크기).
template <class E>
consteval bool CheckEnumTableShape() noexcept {
    static_assert(IsNameTable<E> || IsEntryTable<E>,
                  "CVarEnumNames(E) must return Span<const StringView> or "
                  "Span<const cvar::EnumEntry>");
    if constexpr (requires { E::Count; }) {
        static_assert(EnumTable<E>().Size() == static_cast<usize>(E::Count),
                      "CVarEnumNames(E) size must equal E::Count");
    }

    return true;
}

// enum CVar 타입 E의 이름 조건을 확인한다(VarBuilder가 E에 대해 한 번 평가한다).
template <class E>
consteval bool CheckEnumNames() noexcept {
    static_assert(HasEnumNames<E>, "enum CVar needs names: mark the enum ROCKS_ENUM() or define "
                                   "constexpr CVarEnumNames(E) in the enum's namespace");
    if constexpr (NamedEnum<E> && !ReflectedEnum<E>) {
        return CheckEnumTableShape<E>();
    } else {
        return true;
    }
}

// 열거자 이름 철자([A-Za-z0-9_]+)인지다.
constexpr bool IsEnumeratorName(StringView name) noexcept {
    if (name.IsEmpty()) {
        return false;
    }

    for (const char c : name) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter && !(c >= '0' && c <= '9') && c != '_') {
            return false;
        }
    }

    return true;
}

// E의 기저 정수 값 범위 안인지다.
template <class E>
constexpr bool FitsUnderlying(int64 value) noexcept {
    using Underlying = UnderlyingTypeT<E>;
    if constexpr (IsSameV<Underlying, uint64>) {
        return value >= 0;
    } else {
        return value >= static_cast<int64>(NumericLimits<Underlying>::Min()) &&
               value <= static_cast<int64>(NumericLimits<Underlying>::Max());
    }
}

// ADL 표가 쓸 수 있는지다: 비어 있지 않고, 이름이 열거자 철자이며 ASCII 대소문자를 무시해 서로
// 다르고, 값이 서로 다르며 기저 타입 범위 안이다. 리플렉션 enum은 MetaCompiler가 같은 규칙을 이미
// 검사했으므로 늘 참이다.
template <class E>
constexpr bool IsValidEnumTable() noexcept {
    if constexpr (ReflectedEnum<E>) {
        return true;
    } else {
        constexpr usize count = EnumTable<E>().Size();
        if (count == 0) {
            return false;
        }

        for (usize index = 0; index < count; ++index) {
            if (!IsEnumeratorName(EnumTableName<E>(index)) ||
                !FitsUnderlying<E>(EnumTableValue<E>(index))) {
                return false;
            }

            for (usize other = 0; other < index; ++other) {
                if (EqualsIgnoringAsciiCase(EnumTableName<E>(index), EnumTableName<E>(other)) ||
                    EnumTableValue<E>(index) == EnumTableValue<E>(other)) {
                    return false;
                }
            }
        }

        return true;
    }
}

// value가 E의 이름 있는 값인지다.
template <class E>
constexpr bool IsNamedEnumValue(int64 value) noexcept {
    if constexpr (ReflectedEnum<E>) {
        return GeneratedType<E>::IsDeclared(value);
    } else {
        for (usize index = 0; index < EnumTable<E>().Size(); ++index) {
            if (EnumTableValue<E>(index) == value) {
                return true;
            }
        }

        return false;
    }
}

// 기저 정수 타입의 리플렉션 값 종류다(EnumSchema::underlying).
template <class E>
constexpr ValueKind UnderlyingKind() noexcept {
    using Underlying = UnderlyingTypeT<E>;
    if constexpr (IsSameV<Underlying, int8>) {
        return ValueKind::Int8;
    } else if constexpr (IsSameV<Underlying, int16>) {
        return ValueKind::Int16;
    } else if constexpr (IsSameV<Underlying, int32>) {
        return ValueKind::Int32;
    } else if constexpr (IsSameV<Underlying, int64>) {
        return ValueKind::Int64;
    } else if constexpr (IsSameV<Underlying, uint8>) {
        return ValueKind::UInt8;
    } else if constexpr (IsSameV<Underlying, uint16>) {
        return ValueKind::UInt16;
    } else if constexpr (IsSameV<Underlying, uint32>) {
        return ValueKind::UInt32;
    } else {
        static_assert(IsSameV<Underlying, uint64>,
                      "enum CVar underlying type must be a fixed-width integer (int8 to uint64)");
        return ValueKind::UInt64;
    }
}

// ADL 표로 만든 상수 열거자 스키마다. TypeId는 비어 있다(TypeRegistry에 등록하지 않는 enum이다 —
// 값 해석·표시는 이 스키마만 본다).
template <class E>
struct NamedEnumSchema {
    static constexpr usize Count = EnumTable<E>().Size();

    struct Storage {
        EnumeratorSchema values[Count];
    };

    static constexpr Storage Build() noexcept {
        Storage storage{};
        for (usize index = 0; index < Count; ++index) {
            const StringView name = EnumTableName<E>(index);
            storage.values[index].name = Span<const char>(name.Data(), name.ByteCount());
            storage.values[index].value = EnumTableValue<E>(index);
        }

        return storage;
    }

    static constexpr Storage Enumerators = Build();

    static constexpr EnumSchema Schema{TypeId{},
                                       Span<const char>(),
                                       UnderlyingKind<E>(),
                                       0u,
                                       Span<const EnumeratorSchema>(Enumerators.values, Count),
                                       Span<const char>()};

    static const EnumSchema& Get() noexcept {
        return Schema;
    }
};

// 리플렉션 enum 스키마를 이 모듈 안의 함수로 감싼다. Modular에서 GeneratedType<E>::Schema는 다른
// 공유 라이브러리가 export한 함수라(dllimport) 그 주소는 상수식이 아니다 — constinit CVar가 담을 수
// 없다(Docs/Design/BuildTargets.ko.md 3.8절 BS8d5). 감싼 함수의 주소는 이 모듈 안의 상수다.
template <class E>
const EnumSchema& ReflectedEnumSchema() noexcept {
    return GeneratedType<E>::Schema();
}

// E의 열거자 스키마 함수다.
template <class E>
constexpr EnumSchemaFunction EnumSchemaOf() noexcept {
    if constexpr (ReflectedEnum<E>) {
        return &ReflectedEnumSchema<E>;
    } else {
        return &NamedEnumSchema<E>::Get;
    }
}

} // namespace rocks::cvar::detail
