#pragma once

#include <Base/Compiler.h>
#include <Config/CVarEnum.h>
#include <Config/CVarHandle.h>

// CVar 정의 빌더다(Docs/Design/CVarSystem.ko.md 4.2·4.3절 — CVR1). 진입점(`cvar::Var`·`cvar::Boot`)
// 에서 시작해 `&&` 체인으로 성질을 잇고, 끝의 consteval 변환이 핸들을 만든다. 모든 검사(이름 문법,
// 도움말, 범위 순서, 기본값의 범위·enum 표·`Validate`)는 그 변환 안에서 컴파일 때 끝난다. 위반은
// 컴파일 오류이고 진단에 detail::CVarError_* 표지 함수 이름이 나온다. 타입에 맞지 않는 빌더 호출은
// static_assert, 빌더를 `auto`로 받으면 삭제된 생성자 진단이다.
//
// 사용 예:
//   CVar<int32> Samples = cvar::Var("render.aa.samples", 4).Help("MSAA sample count")
//       .Validate([](const int32& value) noexcept -> StringView {
//           return (value & (value - 1)) == 0 ? StringView{} : "must be a power of two";
//       });
namespace rocks::cvar {

namespace detail {

// 빌더 공통 상태와 끝 검사다.
template <class T>
class BuilderCore {
    static_assert(IsVarType<T>,
                  "CVar type must be bool, int32, int64, float, double, String, Name or an enum "
                  "class");

public:
    BuilderCore(const BuilderCore&) = delete;
    BuilderCore& operator=(const BuilderCore&) = delete;
    BuilderCore(BuilderCore&&) = delete;
    BuilderCore& operator=(BuilderCore&&) = delete;

protected:
    constexpr BuilderCore(StringView name, const ScalarValue& defaultValue,
                          HandleKind kind) noexcept {
        definition_.name = name;
        definition_.type = ValueTypeOf<T>();
        definition_.kind = kind;
        definition_.defaultValue = defaultValue;
    }

    constexpr void SetRange(TypeIdentityT<T> minimum, TypeIdentityT<T> maximum) noexcept {
        if constexpr (IsNumericVarType<T>) {
            definition_.flags |= static_cast<uint32>(VarFlags::HasRange);
            definition_.minimum = ScalarOf<T>(minimum);
            definition_.maximum = ScalarOf<T>(maximum);
        }
    }

    constexpr void AddFlag(VarFlags flag) noexcept {
        definition_.flags |= static_cast<uint32>(flag);
    }

    // 끝 검사다(consteval 변환에서만 부른다). 위반은 표지 함수 호출로 컴파일 오류가 된다.
    consteval TypedDefinition<T> Finish() const noexcept {
        TypedDefinition<T> definition = definition_;
        if (!config::IsConsoleSettingName(definition.name)) {
            CVarError_InvalidName();
        }

        if (definition.help.IsEmpty()) {
            CVarError_MissingHelp();
        }

        const bool ranged = HasFlag(definition.flags, VarFlags::HasRange);
        if (HasFlag(definition.flags, VarFlags::Clamp) && !ranged) {
            CVarError_ClampWithoutRange();
        }

        if constexpr (IsSameV<T, float> || IsSameV<T, double>) {
            // NaN 끝도 여기서 걸린다.
            if (ranged && !(definition.minimum.real <= definition.maximum.real)) {
                CVarError_RangeMinimumAboveMaximum();
            }
        } else if constexpr (IsNumericVarType<T>) {
            if (ranged && definition.minimum.integer > definition.maximum.integer) {
                CVarError_RangeMinimumAboveMaximum();
            }
        }

        if constexpr (IsEnumV<T>) {
            if (!IsValidEnumTable<T>()) {
                CVarError_InvalidEnumNames();
            }

            if (!IsNamedEnumValue<T>(definition.defaultValue.integer)) {
                CVarError_DefaultNotInEnum();
            }

            definition.enumSchema = EnumSchemaOf<T>();
        }

        definition.check = &CheckValue<T>;
        ScalarValue probe = definition.defaultValue;
        StringView reason;
        switch (CheckValue<T>(definition, probe, reason)) {
        case ValueCheck::Accepted:
            break;
        case ValueCheck::Clamped:
        case ValueCheck::OutOfRange:
            CVarError_DefaultOutOfRange();
            break;
        case ValueCheck::ValidationFailed:
            CVarError_DefaultFailsValidate();
            break;
        }

        // 출시 빌드의 치트 변수는 이름·도움말·검사 함수를 버린다(실행 파일에 남지 않는다 — 13.1절).
        // Register는 이런 정의를 건너뛴다.
        if constexpr (IsReleaseBuild) {
            if (HasFlag(definition.flags, VarFlags::Cheat)) {
                definition.name = StringView{};
                definition.help = StringView{};
                definition.category = StringView{};
                definition.validate = nullptr;
            }
        }

        return definition;
    }

    TypedDefinition<T> definition_{};
};

// enum이면 이름 표 조건을 확인한다(enum이 아니면 늘 참).
template <class T>
consteval bool CheckVarTypeNames() noexcept {
    if constexpr (IsEnumV<T>) {
        return CheckEnumNames<T>();
    } else {
        return true;
    }
}

} // namespace detail

// `cvar::Var`가 돌려주는 빌더다. 복사·이동할 수 없고 모든 메서드가 rvalue에만 있으므로 정의 문장
// 하나 안에서만 쓴다. 끝에서 CVar<T>로 바뀐다(consteval).
template <class T>
class VarBuilder : public detail::BuilderCore<T> {
    static_assert(detail::CheckVarTypeNames<T>());

public:
    constexpr VarBuilder(StringView name, const detail::ScalarValue& defaultValue) noexcept
        : detail::BuilderCore<T>(name, defaultValue, HandleKind::Var) {
    }

    // 도움말이다(필수 — 없으면 컴파일 오류).
    constexpr VarBuilder&& Help(StringView text) && noexcept {
        this->definition_.help = text;
        return static_cast<VarBuilder&&>(*this);
    }

    // cvar.find 분류다.
    constexpr VarBuilder&& Category(StringView category) && noexcept {
        this->definition_.category = category;
        return static_cast<VarBuilder&&>(*this);
    }

    // 검사 함수다(ValidateFunction — 갈무리 없는 람다도 된다). 기본값도 컴파일 때 이 함수로
    // 검사한다.
    constexpr VarBuilder&& Validate(ValidateFunction<T> check) && noexcept {
        this->definition_.validate = check;
        return static_cast<VarBuilder&&>(*this);
    }

    // 닫힌 범위다(수 타입만). 밖의 값은 거부한다(`Clamp`면 보정). minimum > maximum은 컴파일
    // 오류다.
    constexpr VarBuilder&& Range(TypeIdentityT<T> minimum, TypeIdentityT<T> maximum) && noexcept {
        static_assert(IsNumericVarType<T>,
                      "Range() is only available for numeric CVars (int32, int64, float, double)");
        this->SetRange(minimum, maximum);
        return static_cast<VarBuilder&&>(*this);
    }

    // 범위 밖 값을 범위 끝으로 보정해 받고 Clamped 진단을 남긴다(Range가 있어야 한다).
    constexpr VarBuilder&& Clamp() && noexcept {
        static_assert(IsNumericVarType<T>,
                      "Clamp() is only available for numeric CVars (int32, int64, float, double)");
        this->AddFlag(VarFlags::Clamp);
        return static_cast<VarBuilder&&>(*this);
    }

    // 치트 변수다(출시 빌드에서 등록되지 않는다 — 13.1절).
    constexpr VarBuilder&& Cheat() && noexcept {
        this->AddFlag(VarFlags::Cheat);
        return static_cast<VarBuilder&&>(*this);
    }

    // 서버가 복제하는 값이다(13.2절).
    constexpr VarBuilder&& Replicated() && noexcept {
        this->AddFlag(VarFlags::Replicated);
        return static_cast<VarBuilder&&>(*this);
    }

    // 사용자 층(사용자 설정 파일)에 쓸 수 있다.
    constexpr VarBuilder&& UserSetting() && noexcept {
        this->AddFlag(VarFlags::UserSetting);
        return static_cast<VarBuilder&&>(*this);
    }

    // 콘솔 층에서 바꿀 수 없다(다른 층은 된다).
    constexpr VarBuilder&& ConsoleReadOnly() && noexcept {
        this->AddFlag(VarFlags::ConsoleReadOnly);
        return static_cast<VarBuilder&&>(*this);
    }

    // 핸들로 바뀐다. consteval이라 모든 검사가 컴파일 때 끝난다.
    consteval operator CVar<T>() && noexcept {
        return CVar<T>(this->Finish());
    }

    // 다른 타입의 핸들로는 바뀌지 않는다(`CVar<int32> X = cvar::Var("x", 1.f)`).
    template <class Other>
        requires(!IsSameV<Other, T>)
    consteval operator CVar<Other>() && noexcept {
        static_assert(detail::DependentFalse<Other>, "CVar type must match the default value type");
    }
};

// `cvar::Boot`가 돌려주는 빌더다. Help·Category·Range·Validate·UserSetting만 있다(Clamp·Cheat·
// Replicated·ConsoleReadOnly를 부르면 컴파일 오류 — 4.2절). 끝에서 BootCVar<T>로 바뀐다.
template <class T>
class BootVarBuilder : public detail::BuilderCore<T> {
    static_assert(detail::CheckVarTypeNames<T>());

public:
    constexpr BootVarBuilder(StringView name, const detail::ScalarValue& defaultValue) noexcept
        : detail::BuilderCore<T>(name, defaultValue, HandleKind::Boot) {
    }

    constexpr BootVarBuilder&& Help(StringView text) && noexcept {
        this->definition_.help = text;
        return static_cast<BootVarBuilder&&>(*this);
    }

    constexpr BootVarBuilder&& Category(StringView category) && noexcept {
        this->definition_.category = category;
        return static_cast<BootVarBuilder&&>(*this);
    }

    constexpr BootVarBuilder&& Validate(ValidateFunction<T> check) && noexcept {
        this->definition_.validate = check;
        return static_cast<BootVarBuilder&&>(*this);
    }

    constexpr BootVarBuilder&& Range(TypeIdentityT<T> minimum,
                                     TypeIdentityT<T> maximum) && noexcept {
        static_assert(IsNumericVarType<T>,
                      "Range() is only available for numeric CVars (int32, int64, float, double)");
        this->SetRange(minimum, maximum);
        return static_cast<BootVarBuilder&&>(*this);
    }

    // 사용자가 다음 실행용 값을 사용자 층에 저장할 수 있다(VD15 — 재시작 대기 목록에 오른다).
    constexpr BootVarBuilder&& UserSetting() && noexcept {
        this->AddFlag(VarFlags::UserSetting);
        return static_cast<BootVarBuilder&&>(*this);
    }

    consteval operator BootCVar<T>() && noexcept {
        return BootCVar<T>(this->Finish());
    }

    template <class Other>
        requires(!IsSameV<Other, T>)
    consteval operator BootCVar<Other>() && noexcept {
        static_assert(detail::DependentFalse<Other>,
                      "BootCVar type must match the default value type");
    }
};

// CVar 정의의 진입점이다. T는 기본값에서 추론한다(bool·int32·int64·float·double·enum class).
//
// 사용 예:
//   CVar<bool> GodMode = cvar::Var("game.god", false).Help("Invulnerable player").Cheat();
template <class T>
    requires(!IsSameV<T, const char*>)
constexpr VarBuilder<T> Var(StringView name, T defaultValue) noexcept {
    if constexpr (IsVarType<T> && !IsTextVarType<T>) {
        return VarBuilder<T>(name, detail::ScalarOf<T>(defaultValue));
    } else {
        return VarBuilder<T>(name, detail::ScalarValue{});
    }
}

// 문자열 리터럴 기본값의 String 변수다(일반 템플릿은 const char*로 추론하므로 따로 둔다).
template <usize Count>
constexpr VarBuilder<String> Var(StringView name, const char (&defaultValue)[Count]) noexcept {
    detail::ScalarValue scalar;
    scalar.text = StringView(defaultValue, Count - 1);
    return VarBuilder<String>(name, scalar);
}

// Name 변수다(`cvar::Var<Name>("x", "Default")`). Name은 pool 토큰이라 상수로 만들 수 없으므로
// 기본값 텍스트를 담고 Register가 문맥의 NamePool에 intern한다.
template <class T>
    requires IsSameV<T, rocks::Name>
constexpr VarBuilder<rocks::Name> Var(StringView name, StringView defaultValue) noexcept {
    detail::ScalarValue scalar;
    scalar.text = defaultValue;
    return VarBuilder<rocks::Name>(name, scalar);
}

// 부팅 고정 변수 정의의 진입점이다(9절). 오버로드는 Var와 같다.
//
// 사용 예:
//   BootCVar<String> GraphicsBackend = cvar::Boot("render.backend", "vulkan").Help("Graphics
//   backend");
template <class T>
    requires(!IsSameV<T, const char*>)
constexpr BootVarBuilder<T> Boot(StringView name, T defaultValue) noexcept {
    if constexpr (IsVarType<T> && !IsTextVarType<T>) {
        return BootVarBuilder<T>(name, detail::ScalarOf<T>(defaultValue));
    } else {
        return BootVarBuilder<T>(name, detail::ScalarValue{});
    }
}

template <usize Count>
constexpr BootVarBuilder<String> Boot(StringView name, const char (&defaultValue)[Count]) noexcept {
    detail::ScalarValue scalar;
    scalar.text = StringView(defaultValue, Count - 1);
    return BootVarBuilder<String>(name, scalar);
}

template <class T>
    requires IsSameV<T, rocks::Name>
constexpr BootVarBuilder<rocks::Name> Boot(StringView name, StringView defaultValue) noexcept {
    detail::ScalarValue scalar;
    scalar.text = defaultValue;
    return BootVarBuilder<rocks::Name>(name, scalar);
}

} // namespace rocks::cvar
