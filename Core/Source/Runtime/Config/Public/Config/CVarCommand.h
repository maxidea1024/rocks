#pragma once

#include <Config/CVarBuilder.h>

// 콘솔 명령이다(Docs/Design/CVarSystem.ko.md 4.2·13·15절 — CVR6, UE FAutoConsoleCommand 대응).
// 명령은 `cvar::Command<&Function>(이름)` 빌더 한 문장으로 정의하는 상수 초기화 핸들이고, 모듈 등록
// 함수에서 ConfigContext::Register로 올린다(CVar와 섞어도 된다 — Policy cvar-registration 대상).
// 콘솔 입력
// `<명령> <인자…>`는 큐에 들어가 Sync 5단계(값 적용·게시 뒤, OnSynced 전)에 게임 lane이 실행하고,
// 출력은 입력 때 받은 sink로 간다.
//
// 인자는 함수 매개변수 타입에서 읽는다: 공백으로 나눈 토큰마다 ReflectionText 명령행 단일 값
// 표기(bool·정수·실수·enum 이름 또는 수), 마지막 `StringView` 매개변수는 줄의 나머지(비어도 된다)를
// 받는다.
//
// 사용 예:
//   Result<void> Teleport(cvar::CommandContext& context, float x, float y, float z) noexcept;
//
//   constinit ConsoleCommand TeleportCommand = cvar::Command<&Teleport>("cheat.teleport")
//       .Help("Teleport the local player").Cheat();
//
//   // 콘솔: cheat.teleport 0 0 100
//
// 명령이 객체(제어기 등)를 다뤄야 하면 ConfigContext::BindCommand(TeleportCommand, player)로 대상을
// 묶고 함수 안에서 context.Target<Player>()로 받는다.
namespace rocks::config {

class ConfigContext;

} // namespace rocks::config

namespace rocks::cvar {

// 명령 하나가 받을 수 있는 매개변수 수다.
inline constexpr usize MaxCommandArguments = 8;

namespace detail {

// 명령 대상 타입의 표지다(타입마다 주소가 하나 — BindCommand가 기록하고 Target<T>가 확인한다).
template <class T>
inline constexpr char TargetTypeTag = 0;

// 명령 대상을 묶을 때와 다른 타입으로 꺼냈다(Fatal).
[[noreturn]] ROCKS_CONFIG_API void FatalTargetType(StringView command) noexcept;

} // namespace detail

// 명령 함수가 받는 실행 문맥이다(게임 lane — Sync 5단계). 출력 sink·문맥·묶인 대상을 준다. 호출
// 동안만 유효하다.
class CommandContext {
public:
    CommandContext(config::ConfigContext& config, config::ConsoleSink* sink, void* target,
                   const void* targetType, StringView command, Allocator& allocator) noexcept
        : config_(&config), sink_(sink), target_(target), targetType_(targetType),
          command_(command), allocator_(&allocator) {
    }

    // 명령을 실행하는 설정 문맥이다(게임 lane — 값 읽기·쓰기 큐에 넣기).
    config::ConfigContext& Config() const noexcept {
        return *config_;
    }

    // 입력 때 받은 sink에 text를 그대로 쓴다(sink가 없으면 무동작). 오류: sink의 오류.
    Result<void> Write(StringView text) const noexcept {
        return sink_ == nullptr ? Result<void>::Success() : sink_->Write(text);
    }

    // ConfigContext::BindCommand로 묶은 대상이다(묶지 않았으면 null). T가 묶을 때의 타입과 다르면
    // Fatal이다(잘못된 형 변환을 조용히 넘기지 않는다).
    template <class T>
    T* Target() const noexcept {
        if (target_ != nullptr && targetType_ != &detail::TargetTypeTag<T>) {
            detail::FatalTargetType(command_);
        }

        return static_cast<T*>(target_);
    }

    // 명령 안에서 쓰는 임시 할당자(문맥 allocator)다.
    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

private:
    config::ConfigContext* config_;
    config::ConsoleSink* sink_;
    void* target_;
    const void* targetType_;
    StringView command_;
    Allocator* allocator_;
};

namespace detail {

// 명령 매개변수로 받는 타입인지다.
template <class T>
inline constexpr bool IsCommandArgument =
    IsSameV<T, bool> || IsNumericVarType<T> || IsSameV<T, StringView> || IsScopedEnumV<T>;

// 매개변수 하나의 값 종류다(StringView는 String).
template <class T>
constexpr config::ConsoleValueType CommandArgumentType() noexcept {
    if constexpr (IsSameV<T, StringView>) {
        return config::ConsoleValueType::String;
    } else {
        return ValueTypeOf<T>();
    }
}

// 읽은 값(FieldValue)을 매개변수 타입으로 바꾼다.
template <class T>
T CommandArgumentOf(const FieldValue& value) noexcept {
    if constexpr (IsSameV<T, bool>) {
        return value.boolean;
    } else if constexpr (IsSameV<T, float>) {
        return value.float32;
    } else if constexpr (IsSameV<T, double>) {
        return value.float64;
    } else if constexpr (IsSameV<T, StringView>) {
        return value.string.View();
    } else {
        return static_cast<T>(value.signedInteger);
    }
}

// 매개변수 하나의 설명이다.
struct CommandArgument {
    config::ConsoleValueType type = config::ConsoleValueType::Bool;
    EnumSchemaFunction enumSchema = nullptr;
};

// 서명을 지운 호출 함수다. arguments는 매개변수 수만큼 읽은 값이다.
using CommandInvokeFunction = Result<void> (*)(CommandContext& context,
                                               const FieldValue* arguments) noexcept;

// 명령의 상수 정의다(공통 머리 VarDefinition — kind가 Command, 이름·도움말·분류·Cheat 플래그 — 위에
// 매개변수 표와 호출 함수).
struct CommandDefinition : VarDefinition {
    uint32 argumentCount = 0;
    CommandArgument arguments[MaxCommandArguments]{};
    // 마지막 매개변수가 StringView면 줄의 나머지를 받는다.
    bool takesRest = false;
    CommandInvokeFunction invoke = nullptr;
};

template <class Signature>
struct CommandSignature {
    static constexpr bool Valid = false;
};

template <class... Args>
struct CommandSignature<Result<void> (*)(CommandContext&, Args...) noexcept> {
    static constexpr bool Valid = true;
    static constexpr usize Count = sizeof...(Args);
    static constexpr bool Supported = (IsCommandArgument<Args> && ...);

    template <auto Function, usize... Indices>
    static Result<void> Call(CommandContext& context, const FieldValue* arguments,
                             IndexSequence<Indices...>) noexcept {
        return Function(context, CommandArgumentOf<Args>(arguments[Indices])...);
    }

    template <auto Function>
    static Result<void> Invoke(CommandContext& context, const FieldValue* arguments) noexcept {
        return Call<Function>(context, arguments, IndexSequenceFor<Args...>{});
    }

    static constexpr void Describe(CommandDefinition& definition) noexcept {
        definition.argumentCount = static_cast<uint32>(Count);
        usize index = 0;
        ((definition.arguments[index].type = CommandArgumentType<Args>(),
          definition.arguments[index].enumSchema = EnumSchemaOfArgument<Args>(), ++index),
         ...);
        if constexpr (Count != 0) {
            definition.takesRest = IsSameV<LastOf<Args...>, StringView>;
        }
    }

private:
    template <class T>
    static constexpr EnumSchemaFunction EnumSchemaOfArgument() noexcept {
        if constexpr (IsScopedEnumV<T>) {
            return EnumSchemaOf<T>();
        } else {
            return nullptr;
        }
    }

    template <class... Types>
    struct Last;

    template <class Only>
    struct Last<Only> {
        using Type = Only;
    };

    template <class First, class Second, class... Rest>
    struct Last<First, Second, Rest...> {
        using Type = typename Last<Second, Rest...>::Type;
    };

    template <class... Types>
    using LastOf = typename Last<Types...>::Type;
};

// 명령 함수를 부르는 서명별 호출 함수다.
template <auto Function>
Result<void> InvokeCommand(CommandContext& context, const FieldValue* arguments) noexcept {
    return CommandSignature<decltype(Function)>::template Invoke<Function>(context, arguments);
}

// enum 매개변수면 이름 표 조건을 확인한다.
template <class T>
consteval bool CheckCommandArgumentNames() noexcept {
    if constexpr (IsScopedEnumV<T>) {
        return CheckEnumNames<T>();
    } else {
        return true;
    }
}

template <class Signature>
struct CommandNameCheck {
    static constexpr bool Value = true;
};

template <class... Args>
struct CommandNameCheck<Result<void> (*)(CommandContext&, Args...) noexcept> {
    static constexpr bool Value = (CheckCommandArgumentNames<Args>() && ...);
};

} // namespace detail

template <auto Function>
class CommandBuilder;

// 콘솔 명령 핸들이다(4절 정의 원칙 — 상수 초기화, 정적 저장 기간, 복사·이동·힙 생성 불가). 이름으로
// 부르는 동작이고 값이 없다. 등록은 ConfigContext::Register, 대상 객체 묶기는
// ConfigContext::BindCommand다. 출시 빌드의 `Cheat()` 명령은 이름·호출 함수가 없고 등록되지 않는다.
class ConsoleCommand {
public:
    ConsoleCommand(const ConsoleCommand&) = delete;
    ConsoleCommand& operator=(const ConsoleCommand&) = delete;
    ConsoleCommand(ConsoleCommand&&) = delete;
    ConsoleCommand& operator=(ConsoleCommand&&) = delete;

    static void* operator new(usize) = delete;
    static void* operator new[](usize) = delete;
    static void* operator new(usize, void*) = delete;
    static void* operator new[](usize, void*) = delete;

    constexpr StringView Name() const noexcept {
        return definition_.name;
    }

    constexpr StringView Help() const noexcept {
        return definition_.help;
    }

private:
    friend struct detail::HandleAccess;
    template <auto Function>
    friend class CommandBuilder;

    constexpr explicit ConsoleCommand(const detail::CommandDefinition& definition) noexcept
        : definition_(definition) {
    }

    detail::CommandDefinition definition_;
};

// `cvar::Command`가 돌려주는 빌더다. 복사·이동할 수 없고 rvalue 체인으로만 쓴다. 끝의 consteval
// 변환이 이름 문법·도움말·텍스트 규칙을 검사한다(CVar와 같은 표지 함수). 함수 서명과 매개변수
// 타입은 static_assert다.
template <auto Function>
class CommandBuilder {
    using Signature = detail::CommandSignature<decltype(Function)>;
    static_assert(Signature::Valid,
                  "Command function must be Result<void> (*)(cvar::CommandContext&, Args...) "
                  "noexcept");

public:
    CommandBuilder(const CommandBuilder&) = delete;
    CommandBuilder& operator=(const CommandBuilder&) = delete;
    CommandBuilder(CommandBuilder&&) = delete;
    CommandBuilder& operator=(CommandBuilder&&) = delete;

    constexpr explicit CommandBuilder(StringView name) noexcept {
        if constexpr (Signature::Valid) {
            static_assert(Signature::Count <= MaxCommandArguments,
                          "A console command takes at most MaxCommandArguments parameters");
            static_assert(Signature::Supported,
                          "Command parameters must be bool, int32, int64, float, double, "
                          "StringView or a named enum class");
            static_assert(detail::CommandNameCheck<decltype(Function)>::Value);
            definition_.name = name;
            definition_.kind = HandleKind::Command;
            Signature::Describe(definition_);
            definition_.invoke = &detail::InvokeCommand<Function>;
        }
    }

    // 도움말이다(필수 — 없으면 컴파일 오류).
    constexpr CommandBuilder&& Help(StringView text) && noexcept {
        definition_.help = text;
        return static_cast<CommandBuilder&&>(*this);
    }

    // cvar.find 분류다.
    constexpr CommandBuilder&& Category(StringView category) && noexcept {
        definition_.category = category;
        return static_cast<CommandBuilder&&>(*this);
    }

    // 치트 명령이다: 출시 빌드에서 등록되지 않고, 개발 빌드에서는 치트 허용 상태(`cheat.enabled`)가
    // 꺼져 있으면 실행을 CheatsDisabled로 거부한다(13.1절).
    constexpr CommandBuilder&& Cheat() && noexcept {
        definition_.flags |= static_cast<uint32>(VarFlags::Cheat);
        return static_cast<CommandBuilder&&>(*this);
    }

    consteval operator ConsoleCommand() && noexcept {
        detail::CommandDefinition definition = definition_;
        if (!config::IsConsoleSettingName(definition.name)) {
            detail::CVarError_InvalidName();
        }

        if (definition.help.IsEmpty()) {
            detail::CVarError_MissingHelp();
        }

        if (definition.name.ByteCount() > MaxVarNameBytes ||
            definition.help.ByteCount() > MaxVarHelpBytes ||
            definition.category.ByteCount() > MaxVarCategoryBytes) {
            detail::CVarError_TextTooLong();
        }

        if (!detail::IsDefinitionText(definition.help) ||
            !detail::IsDefinitionText(definition.category)) {
            detail::CVarError_InvalidUtf8();
        }

        // 출시 빌드의 치트 명령은 이름·도움말·호출 함수를 버린다(실행 파일에 남지 않는다 — 13.1절).
        if constexpr (IsReleaseBuild) {
            if (HasFlag(definition.flags, VarFlags::Cheat)) {
                definition.name = StringView{};
                definition.help = StringView{};
                definition.category = StringView{};
                definition.invoke = nullptr;
                for (detail::CommandArgument& argument : definition.arguments) {
                    argument.enumSchema = nullptr;
                }
            }
        }

        return ConsoleCommand(definition);
    }

private:
    detail::CommandDefinition definition_{};
};

// 콘솔 명령 정의의 진입점이다. Function은 `Result<void> (*)(cvar::CommandContext&, Args...)
// noexcept` 함수다(템플릿 인자 — 서명을 지운 호출 함수를 상수로 만들기 위해서다, 4.2절).
//
// 사용 예:
//   constinit ConsoleCommand FlushCommand = cvar::Command<&FlushLogs>("log.flush")
//       .Help("Flush pending log records");
template <auto Function>
constexpr CommandBuilder<Function> Command(StringView name) noexcept {
    return CommandBuilder<Function>(name);
}

namespace detail {

template <>
inline constexpr bool IsHandle<ConsoleCommand> = true;

} // namespace detail

} // namespace rocks::cvar
