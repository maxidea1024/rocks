#include <Core/Algorithms.h>
#include <SparksCompiler/BindingSchema.h>
#include <SparksCompiler/Symbols.h>
#include <SparksCompiler/SyntaxTree.h>
#include "LspServer.h"
#include "LspText.h"

// 의미 강조다(Docs/Design/SparksTools.ko.md 10.7절 — T12).
namespace rocks::sparks::tools::detail {

namespace {

// 종류 번호(LspTokenTypes 순서)다.
enum class LspTokenType : uint32 {
    Namespace,
    Type,
    Class,
    Struct,
    Enum,
    Interface,
    TypeParameter,
    EnumMember,
    Event,
    Function,
    Method,
    Property,
    Parameter,
    Variable
};

// modifier bit(LspTokenModifiers 순서)다.
struct LspModifier {
    static constexpr uint32 Declaration = 1u << 0;
    static constexpr uint32 Readonly = 1u << 1;
    static constexpr uint32 Static = 1u << 2;
    static constexpr uint32 Async = 1u << 3;
    static constexpr uint32 DefaultLibrary = 1u << 4;
};

// 토큰 하나의 data 수(줄 차이·시작 차이·길이·종류·modifier)다.
constexpr usize LspTokenFields = 5;

struct LspTokenEntry {
    uint32 begin = 0;
    uint32 end = 0;
    uint32 type = 0;
    uint32 modifiers = 0;
};

bool LspAsyncSignature(const Compilation& compilation, const Symbol& symbol) noexcept {
    return (symbol.flags & SymbolFlags::Async) != 0 ||
           (symbol.signature != NoSignature && symbol.signature < compilation.Signatures().Size() &&
            compilation.Signatures()[symbol.signature].isAsync);
}

// 심볼의 종류·modifier다. 값 false는 강조하지 않는다.
bool LspClassify(const Compilation& compilation, uint32 symbolIndex, uint32& type,
                 uint32& modifiers) noexcept {
    const Symbol& symbol = compilation.Symbols()[symbolIndex];
    const uint32 library = LspModifier::DefaultLibrary;
    const bool readonly = LspIsReadOnly(compilation, symbol);
    const bool async = LspAsyncSignature(compilation, symbol);
    auto set = [&](LspTokenType kind, uint32 bits) noexcept {
        type = static_cast<uint32>(kind);
        modifiers = bits | (readonly ? LspModifier::Readonly : 0) |
                    (async ? LspModifier::Async : 0) |
                    ((symbol.flags & SymbolFlags::Static) != 0 ? LspModifier::Static : 0);
        return true;
    };
    switch (symbol.kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType:
        return set(LspTokenType::Type, library);
    case SymbolKind::PreludeValue:
        return set(symbol.signature != NoSignature ? LspTokenType::Function
                                                   : LspTokenType::Namespace,
                   library);
    case SymbolKind::ContextName:
        return set(LspTokenType::Variable, library | LspModifier::Readonly);
    case SymbolKind::RpcPrefix:
        return set(LspTokenType::Namespace, library);
    case SymbolKind::EngineType: {
        LspTokenType kind = LspTokenType::Class;
        if (compilation.SchemaIsValid(symbol.index) &&
            symbol.item < compilation.Schema(symbol.index).Types().Size()) {
            switch (compilation.Schema(symbol.index).Types()[symbol.item].kind) {
            case SchemaTypeKind::Struct:
                kind = LspTokenType::Struct;
                break;
            case SchemaTypeKind::Enum:
                kind = LspTokenType::Enum;
                break;
            case SchemaTypeKind::Interface:
                kind = LspTokenType::Interface;
                break;
            default:
                break;
            }
        }

        return set(kind, library);
    }
    case SymbolKind::EngineField:
    case SymbolKind::LanguageField:
        return set(LspTokenType::Property, library);
    case SymbolKind::EngineFunction:
    case SymbolKind::LanguageFunction:
        return set(LspTokenType::Method, library);
    case SymbolKind::EngineEvent:
    case SymbolKind::LanguageEvent:
        return set(LspTokenType::Event, library);
    case SymbolKind::EngineTypeParameter:
    case SymbolKind::LanguageTypeParameter:
        return set(LspTokenType::TypeParameter, library);
    case SymbolKind::Alias:
        return set(LspTokenType::Namespace, 0);
    case SymbolKind::Class:
        return set(LspTokenType::Class, 0);
    case SymbolKind::Struct:
        return set(LspTokenType::Struct, 0);
    case SymbolKind::Enum:
        return set(LspTokenType::Enum, 0);
    case SymbolKind::Interface:
        return set(LspTokenType::Interface, 0);
    case SymbolKind::Const:
        return set(LspTokenType::Variable, LspModifier::Readonly | LspModifier::Static);
    case SymbolKind::Function:
        return set(LspTokenType::Function, LspModifier::Static);
    case SymbolKind::Field:
        return set(LspTokenType::Property, 0);
    case SymbolKind::Method:
    case SymbolKind::InterfaceMethod:
        return set(LspTokenType::Method, 0);
    case SymbolKind::Event:
        return set(LspTokenType::Event, 0);
    case SymbolKind::EnumCase: {
        // 엔진·prelude enum의 case는 기본 라이브러리다.
        const SymbolKind owner =
            symbol.owner != NoSymbol ? compilation.Symbols()[symbol.owner].kind : SymbolKind::Enum;
        const bool provided = owner == SymbolKind::EngineType || owner == SymbolKind::PreludeType ||
                              owner == SymbolKind::BuiltinType;
        return set(LspTokenType::EnumMember, provided ? library : 0);
    }
    case SymbolKind::Parameter:
        return set(LspTokenType::Parameter, 0);
    case SymbolKind::Local:
        return set(LspTokenType::Variable, 0);
    }

    return false;
}

} // namespace

Result<void> LspAppendSemanticTokens(const Compilation& compilation, const SourceSet& sources,
                                     uint32 module, Array<uint32>& data) noexcept {
    const SyntaxTree* tree = compilation.ModuleTree(module);
    if (tree == nullptr) {
        return Result<void>::Success();
    }

    const SourceFile& file = sources.Get(compilation.ModuleSource(module));
    Array<LspTokenEntry> entries(data.GetAllocator());
    auto push = [&](uint32 token, uint32 type, uint32 modifiers) noexcept {
        if (token == NoToken) {
            return Result<void>::Success();
        }

        LspTokenEntry entry;
        entry.begin = tree->Tokens()[token].span.begin;
        entry.end = tree->Tokens()[token].span.end;
        entry.type = type;
        entry.modifiers = modifiers;
        return entries.TryPushBack(entry);
    };
    auto pushSymbol = [&](uint32 node, uint32 symbol) noexcept {
        uint32 type = 0;
        uint32 modifiers = 0;
        if (!LspClassify(compilation, symbol, type, modifiers)) {
            return Result<void>::Success();
        }

        const Symbol& entry = compilation.Symbols()[symbol];
        if (entry.module == module && entry.nameNode == node) {
            modifiers |= LspModifier::Declaration;
        }

        return push(tree->Node(node).token, type, modifiers);
    };

    // 이름 노드 → 심볼 규칙은 위치 질의와 공용이다(LspVisitNames — 11.2절). Type 안의 Path에도
    // 타입이 있으면 같은 토큰이 둘 나오고 정렬 뒤 겹침 제거가 하나만 남긴다.
    auto visit = [&](const LspNamed& named) noexcept {
        if (named.symbol != NoSymbol) {
            return pushSymbol(named.node, named.symbol);
        }

        // import 경로의 이름은 모듈 이름 공간이다.
        return push(named.token, static_cast<uint32>(LspTokenType::Namespace), 0);
    };
    for (uint32 node = 0; node < tree->NodeCount(); ++node) {
        if (auto visited = LspVisitNames(compilation, module, node, visit); !visited) {
            return visited;
        }
    }

    // 원문 위치 순, 겹치면 앞의 것.
    algo::Sort(entries.View(), [](const LspTokenEntry& left, const LspTokenEntry& right) noexcept {
        return left.begin < right.begin;
    });
    LspPosition previous;
    uint32 lastEnd = 0;
    bool first = true;
    for (const LspTokenEntry& entry : entries) {
        if (!first && entry.begin < lastEnd) {
            continue;
        }

        const LspPosition position = LspPositionOf(file, entry.begin);
        const uint32 length =
            LspUtf16Units(StringView(file.Text().Data() + entry.begin, entry.end - entry.begin));
        const uint32 deltaLine = position.line - previous.line;
        const uint32 deltaStart =
            deltaLine == 0 ? position.character - previous.character : position.character;
        const uint32 values[LspTokenFields] = {deltaLine, deltaStart, length, entry.type,
                                               entry.modifiers};
        if (auto appended = data.TryAppend(Span<const uint32>(values, LspTokenFields)); !appended) {
            return appended;
        }

        previous = position;
        lastEnd = entry.end;
        first = false;
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::tools::detail
