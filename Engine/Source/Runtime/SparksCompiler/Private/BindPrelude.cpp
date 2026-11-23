#include <Platform/Fatal.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "NameBinder.h"
#include "PreludeMembers.h"

// 언어 소유 멤버(부록 B 시그니처, SD67)를 심볼로 만든다. 표는 컴파일러 안의 정적 자료이므로 형식이
// 틀리면 진단이 아니라 Fatal이다(시험이 표 전체를 읽어 본다).
// Docs/Design/SparksLanguage 3.8절, 3.10절 "문맥 이름의 타입".

namespace rocks::sparks::detail {

namespace {

constexpr StringView VectorTypes[] = {"vec2", "vec3", "vec4"};

struct Placeholder {
    StringView text;
    LanguageTypeParameterKind kind;
};

constexpr Placeholder Placeholders[] = {
    {"$N", LanguageTypeParameterKind::Numeric}, {"$S", LanguageTypeParameterKind::Signed},
    {"$I", LanguageTypeParameterKind::Integer}, {"$F", LanguageTypeParameterKind::FloatOrVector},
    {"$V", LanguageTypeParameterKind::Vector},
};

// 문맥 이름 → schema 타입 이름(3.10절). Time은 언어 타입이다.
struct ContextType {
    StringView context;
    StringView schemaType;
};

constexpr ContextType ContextTypes[] = {
    {"World", "World"},
    {"Session", "PlaySession"},
    {"Input", "InputSystem"},
    {"Caller", "Connection"},
};

[[noreturn]] void MalformedTable() noexcept {
    platform::Fatal("Sparks prelude member table is malformed");
}

bool IsNameCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '$';
}

} // namespace

// 표기 하나를 읽는다(문법은 schema 표기와 같다).
uint32 NameBinder::ParsePreludeType(StringView text, usize& index,
                                    const PreludeScope& scope) noexcept {
    const usize begin = index;
    while (index < text.ByteCount() && IsNameCharacter(text.Data()[index])) {
        ++index;
    }

    const StringView name(text.Data() + begin, index - begin);
    if (name.IsEmpty()) {
        return Malformed();
    }

    Array<uint32> arguments(allocator_);
    if (index < text.ByteCount() && text.Data()[index] == '<') {
        ++index;
        while (error_ == ErrorCode::None) {
            const uint32 argument = ParsePreludeType(text, index, scope);
            if (auto pushed = arguments.TryPushBack(argument); !pushed) {
                error_ = pushed.Error();
                return NoType;
            }

            if (index + 1 < text.ByteCount() && text.Data()[index] == ',' &&
                text.Data()[index + 1] == ' ') {
                index += 2;
                continue;
            }

            if (index >= text.ByteCount() || text.Data()[index] != '>') {
                return Malformed();
            }

            ++index;
            break;
        }
    }

    const bool optional = index < text.ByteCount() && text.Data()[index] == '?';
    if (optional) {
        ++index;
    }

    uint32 symbol = NoSymbol;
    if (name == "$V" && scope.vector != NoSymbol) {
        symbol = scope.vector;
    } else if (name.Data()[0] == '$') {
        for (uint32 at = 0; at < sizeof(Placeholders) / sizeof(Placeholders[0]); ++at) {
            if (Placeholders[at].text == name) {
                symbol = placeholders_[at];
            }
        }
    } else {
        if (scope.function != NoSymbol) {
            symbol = FindName(SpaceKey(NameSpaceKind::Parameters, scope.function), name);
        }

        if (symbol == NoSymbol && scope.owner != NoSymbol) {
            symbol = FindName(SpaceKey(NameSpaceKind::Parameters, scope.owner), name);
        }

        if (symbol == NoSymbol) {
            const uint32 language = FindName(SpaceKey(NameSpaceKind::Language, 0), name);
            symbol = language != NoSymbol && IsTypeSymbol(language) ? language : NoSymbol;
        }

        if (symbol == NoSymbol) {
            // 엔진이 주는 타입(`Error`). schema에 없으면 그 멤버의 타입은 정해지지 않는다.
            symbol = FindName(SpaceKey(NameSpaceKind::Engine, 0), name);
            if (symbol == NoSymbol) {
                return NoType;
            }
        }
    }

    if (symbol == NoSymbol) {
        return Malformed();
    }

    return InternType(symbol, arguments.View(), optional);
}

// `[<T>](매개변수): [await ]반환` 을 시그니처로 만든다. 함수 자기 타입 매개변수는 function의
// 매개변수 공간에 넣는다.
uint32 NameBinder::ParsePreludeSignature(StringView text, PreludeScope scope,
                                         uint32& flags) noexcept {
    usize index = 0;
    if (index < text.ByteCount() && text.Data()[index] == '<') {
        ++index;
        uint32 position = 0;
        while (index < text.ByteCount() && text.Data()[index] != '>') {
            const usize begin = index;
            while (index < text.ByteCount() && IsNameCharacter(text.Data()[index])) {
                ++index;
            }

            Symbol parameter;
            parameter.kind = SymbolKind::LanguageTypeParameter;
            parameter.name = StringView(text.Data() + begin, index - begin);
            parameter.owner = scope.function;
            parameter.index = static_cast<uint32>(LanguageTypeParameterKind::Generic);
            parameter.item = position++;
            InsertName(SpaceKey(NameSpaceKind::Parameters, scope.function), parameter.name,
                       AddSymbol(parameter));
            if (index + 1 < text.ByteCount() && text.Data()[index] == ',') {
                index += 2;
            }
        }

        ++index; // '>'
        compilation_.symbols_[scope.function].arity = position;
    }

    if (index >= text.ByteCount() || text.Data()[index] != '(') {
        return MalformedSignature();
    }

    ++index;
    Array<uint32> parameters(allocator_);
    constexpr StringView VariadicMark = "...";
    if (StringView(text.Data() + index, text.ByteCount() - index)
            .StartsWith(VariadicMark, StringComparison::CaseSensitive)) {
        flags |= SymbolFlags::Variadic;
        index += VariadicMark.ByteCount();
    }

    while (index < text.ByteCount() && text.Data()[index] != ')') {
        while (index < text.ByteCount() && text.Data()[index] != ':') {
            ++index; // 매개변수 이름
        }

        index += 2; // ": "
        const uint32 type = ParsePreludeType(text, index, scope);
        if (auto pushed = parameters.TryPushBack(type); !pushed) {
            error_ = pushed.Error();
            return NoSignature;
        }

        if (index + 1 < text.ByteCount() && text.Data()[index] == ',') {
            index += 2;
        }
    }

    if (index >= text.ByteCount()) {
        return MalformedSignature();
    }

    ++index; // ')'
    uint32 returnType = voidType_;
    bool isAsync = false;
    if (index < text.ByteCount()) {
        constexpr StringView ReturnMark = ": ";
        constexpr StringView AwaitMark = "await ";
        const StringView rest(text.Data() + index, text.ByteCount() - index);
        if (!rest.StartsWith(ReturnMark, StringComparison::CaseSensitive)) {
            return MalformedSignature();
        }

        index += ReturnMark.ByteCount();
        if (StringView(text.Data() + index, text.ByteCount() - index)
                .StartsWith(AwaitMark, StringComparison::CaseSensitive)) {
            isAsync = true;
            index += AwaitMark.ByteCount();
        }

        returnType = ParsePreludeType(text, index, scope);
        if (index != text.ByteCount()) {
            return MalformedSignature();
        }
    }

    if (isAsync) {
        flags |= SymbolFlags::Async;
    }

    return AddSignature(returnType, parameters.View(), isAsync);
}

void NameBinder::DeclarePreludeMember(const PreludeMember& member, uint32 index, uint32 owner,
                                      uint32 vector) noexcept {
    Compilation& c = compilation_;
    PreludeScope scope;
    scope.owner = owner;
    scope.vector = vector;
    Symbol symbol;
    symbol.name = member.name;
    symbol.owner = owner;
    symbol.index = static_cast<uint32>(member.condition);
    symbol.item = index;
    switch (member.form) {
    case PreludeMemberForm::Field:
        symbol.kind = SymbolKind::LanguageField;
        break;
    case PreludeMemberForm::Property:
    case PreludeMemberForm::Constant:
        symbol.kind = SymbolKind::LanguageField;
        symbol.flags = SymbolFlags::ReadOnly;
        break;
    case PreludeMemberForm::Method:
        symbol.kind = SymbolKind::LanguageFunction;
        break;
    case PreludeMemberForm::Static:
    case PreludeMemberForm::Function:
        symbol.kind = SymbolKind::LanguageFunction;
        symbol.flags = SymbolFlags::Static;
        break;
    case PreludeMemberForm::Event:
        symbol.kind = SymbolKind::LanguageEvent;
        break;
    case PreludeMemberForm::Case:
        symbol.kind = SymbolKind::EnumCase;
        break;
    }

    const uint32 id = AddSymbol(symbol);
    if (id == NoSymbol) {
        return;
    }

    if (symbol.kind == SymbolKind::LanguageField) {
        usize at = 0;
        c.symbols_[id].type = ParsePreludeType(member.signature, at, scope);
    } else if (symbol.kind == SymbolKind::LanguageFunction) {
        scope.function = id;
        uint32 flags = c.symbols_[id].flags;
        const uint32 signature = ParsePreludeSignature(member.signature, scope, flags);
        c.symbols_[id].flags = flags;
        c.symbols_[id].signature = signature;
    } else if (symbol.kind == SymbolKind::LanguageEvent) {
        c.symbols_[id].signature = AddSignature(voidType_, Span<const uint32>(), false);
    }

    InsertName(SpaceKey(NameSpaceKind::Members, owner), member.name, id);
}

// 부록 B 시그니처 표를 심볼로 만들고 문맥 이름의 타입을 정한다(엔진 schema 해석 뒤에 부른다 —
// `Error`·`World` 등 schema 타입을 쓴다).
void NameBinder::DeclarePreludeMembers() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    Compilation& c = compilation_;
    const uint64 language = SpaceKey(NameSpaceKind::Language, 0);
    const uint64 engine = SpaceKey(NameSpaceKind::Engine, 0);
    // 자리표시.
    for (uint32 at = 0; at < sizeof(Placeholders) / sizeof(Placeholders[0]); ++at) {
        Symbol placeholder;
        placeholder.kind = SymbolKind::LanguageTypeParameter;
        placeholder.name = Placeholders[at].text;
        placeholder.index = static_cast<uint32>(Placeholders[at].kind);
        placeholders_[at] = AddSymbol(placeholder);
    }

    // 제네릭 prelude 타입의 타입 인자(Map은 K·V, 나머지는 T).
    for (const LanguageName& entry : LanguageNames()) {
        if (entry.arity == 0) {
            continue;
        }

        const uint32 owner = FindName(language, entry.name);
        constexpr StringView Single[] = {"T"};
        constexpr StringView Pair[] = {"K", "V"};
        const StringView* names = entry.arity == 1 ? Single : Pair;
        for (uint32 position = 0; position < entry.arity; ++position) {
            Symbol parameter;
            parameter.kind = SymbolKind::LanguageTypeParameter;
            parameter.name = names[position];
            parameter.owner = owner;
            parameter.index = static_cast<uint32>(LanguageTypeParameterKind::Generic);
            parameter.item = position;
            InsertName(SpaceKey(NameSpaceKind::Parameters, owner), parameter.name,
                       AddSymbol(parameter));
        }
    }

    // 문맥 이름 Time의 타입(사용자에게 보이는 이름이 아니므로 언어 이름 공간에 넣지 않는다).
    Symbol time;
    time.kind = SymbolKind::PreludeType;
    time.name = TimeTypeName;
    time.index = NoSymbol; // 언어 이름 표에 없는 타입
    timeType_ = AddSymbol(time);
    const Span<const PreludeMember> members = PreludeMembers();
    for (uint32 index = 0; index < members.Size() && error_ == ErrorCode::None; ++index) {
        const PreludeMember& member = members[index];
        if (member.owner.IsEmpty()) {
            // 최상위 prelude 함수(`Assert`·`Delay` 등)는 그 prelude 값 심볼 자신이 함수다.
            const uint32 id = FindName(language, member.name);
            if (id == NoSymbol) {
                Malformed();
                return;
            }

            PreludeScope scope;
            scope.function = id;
            uint32 flags = c.symbols_[id].flags | SymbolFlags::Static;
            const uint32 signature = ParsePreludeSignature(member.signature, scope, flags);
            c.symbols_[id].flags = flags;
            c.symbols_[id].signature = signature;
            c.symbols_[id].item = index;
            continue;
        }

        if (member.owner == VectorFamilyOwner) {
            for (const StringView vector : VectorTypes) {
                const uint32 owner = FindName(language, vector);
                DeclarePreludeMember(member, index, owner, owner);
            }

            continue;
        }

        const uint32 owner =
            member.owner == TimeTypeName ? timeType_ : FindName(language, member.owner);
        if (owner == NoSymbol) {
            Malformed();
            return;
        }

        DeclarePreludeMember(member, index, owner, NoSymbol);
    }

    // 문맥 이름의 타입(3.10절).
    for (uint32 at = 0; at < sizeof(ContextNames) / sizeof(ContextNames[0]); ++at) {
        const uint32 context = firstContextSymbol_ + at;
        if (ContextNames[at] == "Time") {
            c.symbols_[context].type = InternType(timeType_, Span<const uint32>(), false);
            continue;
        }

        for (const ContextType& entry : ContextTypes) {
            if (entry.context == ContextNames[at]) {
                const uint32 type = FindName(engine, entry.schemaType);
                c.symbols_[context].type =
                    type == NoSymbol ? NoType : InternType(type, Span<const uint32>(), false);
            }
        }
    }
}

// 표 형식 오류는 Fatal이다. 할당 실패로 앞 단계가 멈춰 이름을 찾지 못한 경우는 형식 오류가 아니므로
// 조용히 끝낸다(error_가 이미 있다).
uint32 NameBinder::Malformed() noexcept {
    if (error_ == ErrorCode::None) {
        MalformedTable();
    }

    return NoType;
}

uint32 NameBinder::MalformedSignature() noexcept {
    static_cast<void>(Malformed());
    return NoSignature;
}

} // namespace rocks::sparks::detail
