#include <SparksCompiler/BindingSchema.h>
#include "LspServer.h"

// 커서의 이름·선언 모양·hover·정의 이동이다(Docs/Design/SparksTools.ko.md 11.2–11.4절).
namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView LspEnginePrefix = "Rocks.";
constexpr StringView LspDocPrefix = "///";
constexpr StringView LspVoidText = "void";

bool LspIsTypeSymbol(SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType:
    case SymbolKind::PreludeValue:
    case SymbolKind::EngineType:
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
        return true;
    default:
        return false;
    }
}

bool LspIsEngine(SymbolKind kind) noexcept {
    return kind == SymbolKind::EngineType || kind == SymbolKind::EngineField ||
           kind == SymbolKind::EngineFunction || kind == SymbolKind::EngineEvent;
}

// 타입의 짧은 표기다(11.3절): 내장 타입은 정규 이름, 그 밖은 타입 심볼 이름 + 타입 인자 + `?`.
void LspAppendType(const Compilation& compilation, uint32 type, StringBuilderBase& out) noexcept {
    if (type == NoType || type >= compilation.Types().Size()) {
        out.Append("?");
        return;
    }

    const TypeEntry& entry = compilation.Types()[type];
    if (entry.symbol == NoSymbol ||
        compilation.Symbols()[entry.symbol].kind == SymbolKind::BuiltinType) {
        static_cast<void>(compilation.AppendTypeText(out, type));
        return;
    }

    out.Append(compilation.Symbols()[entry.symbol].name);
    if (entry.argumentCount > 0) {
        out.Append("<");
        for (uint32 at = 0; at < entry.argumentCount; ++at) {
            out.Append(at == 0 ? "" : ", ");
            LspAppendType(compilation, compilation.TypeArguments()[entry.firstArgument + at], out);
        }

        out.Append(">");
    }

    if (entry.optional) {
        out.Append("?");
    }
}

bool LspIsAsync(const Compilation& compilation, const Symbol& symbol) noexcept {
    return (symbol.flags & SymbolFlags::Async) != 0 ||
           (symbol.signature != NoSignature && symbol.signature < compilation.Signatures().Size() &&
            compilation.Signatures()[symbol.signature].isAsync);
}

// 멤버의 주인 접두(`Speaker.`·`Rocks.Actor.`)다.
void LspAppendOwner(const Compilation& compilation, const Symbol& symbol,
                    StringBuilderBase& out) noexcept {
    if (symbol.owner == NoSymbol) {
        return;
    }

    const Symbol& owner = compilation.Symbols()[symbol.owner];
    if (!LspIsTypeSymbol(owner.kind)) {
        return;
    }

    if (owner.kind == SymbolKind::EngineType) {
        out.Append(LspEnginePrefix);
    }

    out.Append(owner.name).Append(".");
}

// 함수·이벤트의 매개변수 목록 `(a: int, b: float)`다. 이름은 선언(사용자)·schema(엔진)에서, 언어
// 함수는 타입만.
void LspAppendParameters(const Compilation& compilation, uint32 symbolIndex,
                         StringBuilderBase& out) noexcept {
    const Symbol& symbol = compilation.Symbols()[symbolIndex];
    out.Append("(");
    if (symbol.module != NoModule) {
        // 선언의 매개변수 목록에서(버린 매개변수 `_`는 심볼이 없다 — 자리를 그대로 보인다).
        const SyntaxTree* tree = compilation.ModuleTree(symbol.module);
        uint32 list = NoNode;
        if (tree != nullptr && symbol.node < tree->NodeCount()) {
            const SyntaxNode& declaration = tree->Node(symbol.node);
            const uint32 at = declaration.kind == SyntaxKind::ExtensionEvent ? 4 : 3;
            const bool listed = declaration.kind == SyntaxKind::Function ||
                                declaration.kind == SyntaxKind::InterfaceMethod ||
                                declaration.kind == SyntaxKind::Event ||
                                declaration.kind == SyntaxKind::ExtensionEvent;
            list = listed && declaration.childCount > at ? tree->Child(symbol.node, at) : NoNode;
        }

        if (list != NoNode && tree->Node(list).kind == SyntaxKind::List) {
            bool first = true;
            for (const uint32 parameter : tree->Children(list)) {
                if (parameter == NoNode || tree->Node(parameter).kind != SyntaxKind::Parameter ||
                    tree->Node(parameter).childCount < 2) {
                    continue;
                }

                const uint32 name = tree->Child(parameter, 0);
                const uint32 type = tree->Child(parameter, 1);
                const uint32 named =
                    name == NoNode ? NoSymbol : compilation.NodeSymbol(symbol.module, name);
                out.Append(first ? "" : ", ");
                if (name != NoNode && tree->Node(name).kind == SyntaxKind::Discard) {
                    out.Append("_");
                } else {
                    out.Append(named != NoSymbol ? compilation.Symbols()[named].name
                                                 : StringView("?"));
                }

                out.Append(": ");
                LspAppendType(compilation,
                              type == NoNode ? NoType : compilation.NodeType(symbol.module, type),
                              out);
                first = false;
            }
        }

        out.Append(")");
        return;
    }

    const bool engine = LspIsEngine(symbol.kind) && compilation.SchemaIsValid(symbol.index);
    const BindingSchema* schema = engine ? &compilation.Schema(symbol.index) : nullptr;
    uint32 firstName = 0;
    uint32 names = 0;
    if (schema != nullptr && symbol.kind == SymbolKind::EngineFunction &&
        symbol.item < schema->Functions().Size()) {
        firstName = schema->Functions()[symbol.item].firstParameter;
        names = schema->Functions()[symbol.item].parameterCount;
    } else if (schema != nullptr && symbol.kind == SymbolKind::EngineEvent &&
               symbol.item < schema->Events().Size()) {
        firstName = schema->Events()[symbol.item].firstParameter;
        names = schema->Events()[symbol.item].parameterCount;
    }

    if (symbol.signature != NoSignature && symbol.signature < compilation.Signatures().Size()) {
        const Signature& signature = compilation.Signatures()[symbol.signature];
        for (uint32 at = 0; at < signature.parameterCount; ++at) {
            out.Append(at == 0 ? "" : ", ");
            if (at < names) {
                out.Append(schema->Text(schema->Parameters()[firstName + at].name)).Append(": ");
            }

            LspAppendType(compilation,
                          compilation.SignatureParameters()[signature.firstParameter + at], out);
        }
    } else {
        // 시그니처 없는 엔진 이벤트: schema 표기 그대로.
        for (uint32 at = 0; at < names; ++at) {
            const SchemaParameter& parameter = schema->Parameters()[firstName + at];
            out.Append(at == 0 ? "" : ", ")
                .Append(schema->Text(parameter.name))
                .Append(": ")
                .Append(schema->Text(parameter.type));
        }
    }

    out.Append(")");
}

void LspAppendReturn(const Compilation& compilation, const Symbol& symbol,
                     StringBuilderBase& out) noexcept {
    if (symbol.signature == NoSignature || symbol.signature >= compilation.Signatures().Size()) {
        return;
    }

    // void는 타입 표의 내장 심볼 이름으로 본다(임시 글자를 만들지 않는다 — 할당 실패를 삼키지
    // 않게).
    const uint32 type = compilation.Signatures()[symbol.signature].returnType;
    if (type != NoType && type < compilation.Types().Size()) {
        const TypeEntry& entry = compilation.Types()[type];
        if (entry.symbol != NoSymbol && !entry.optional &&
            compilation.Symbols()[entry.symbol].kind == SymbolKind::BuiltinType &&
            compilation.Symbols()[entry.symbol].name == LspVoidText) {
            return;
        }
    }

    out.Append(": ");
    LspAppendType(compilation, type, out);
}

// 선언 노드의 문서 주석 줄들(`///`와 그 뒤 공백 하나를 뗀)을 out에 붙인다. 값 false는 없음.
bool LspAppendDoc(const Compilation& compilation, const SourceSet& sources, const Symbol& symbol,
                  StringBuilderBase& out) noexcept {
    if (symbol.module == NoModule || symbol.node == NoNode) {
        return false;
    }

    const SyntaxTree* tree = compilation.ModuleTree(symbol.module);
    if (tree == nullptr || symbol.node >= tree->NodeCount()) {
        return false;
    }

    switch (tree->Node(symbol.node).kind) {
    case SyntaxKind::Class:
    case SyntaxKind::Interface:
    case SyntaxKind::InterfaceMethod:
    case SyntaxKind::Struct:
    case SyntaxKind::Enum:
    case SyntaxKind::EnumCase:
    case SyntaxKind::ExtensionEvent:
    case SyntaxKind::Const:
    case SyntaxKind::Function:
    case SyntaxKind::Field:
    case SyntaxKind::Event:
        break;
    default:
        return false;
    }

    const uint32 doc =
        tree->Node(symbol.node).childCount > 0 ? tree->Child(symbol.node, 0) : NoNode;
    if (doc == NoNode || tree->Node(doc).kind != SyntaxKind::DocComment) {
        return false;
    }

    const StringView text = sources.Get(tree->Source()).Text();
    const SyntaxNode& entry = tree->Node(doc);
    for (uint32 at = 0; at < entry.extra; ++at) {
        const uint32 token = entry.token + at;
        if (token >= tree->Tokens().Size()) {
            break;
        }

        const SourceSpan span = tree->Tokens()[token].span;
        StringView line(text.Data() + span.begin, span.end - span.begin);
        if (line.StartsWith(LspDocPrefix, StringComparison::CaseSensitive)) {
            line = StringView(line.Data() + LspDocPrefix.ByteCount(),
                              line.ByteCount() - LspDocPrefix.ByteCount());
        }

        if (line.StartsWith(" ")) {
            line = StringView(line.Data() + 1, line.ByteCount() - 1);
        }

        while (line.EndsWith("\r")) {
            line = StringView(line.Data(), line.ByteCount() - 1);
        }

        out.Append(at == 0 ? "" : "\n").Append(line);
    }

    return entry.extra > 0;
}

} // namespace

bool LspIsReadOnly(const Compilation& compilation, const Symbol& symbol) noexcept {
    if ((symbol.flags & (SymbolFlags::Let | SymbolFlags::ReadOnly)) != 0) {
        return true;
    }

    if (symbol.kind != SymbolKind::EngineField || !compilation.SchemaIsValid(symbol.index)) {
        return false;
    }

    const BindingSchema& schema = compilation.Schema(symbol.index);
    return symbol.item < schema.Fields().Size() && !schema.Fields()[symbol.item].readWrite;
}

bool LspNameAt(const Compilation& compilation, uint32 module, uint32 offset,
               LspNamed& named) noexcept {
    const SyntaxTree* tree = compilation.ModuleTree(module);
    if (tree == nullptr) {
        return false;
    }

    // 위치를 담은 식별자 토큰, 없으면 위치에서 끝나는 식별자 토큰.
    uint32 found = NoToken;
    const Span<const Token> tokens = tree->Tokens();
    for (uint32 at = 0; at < tokens.Size(); ++at) {
        if (tokens[at].kind != TokenKind::Identifier) {
            continue;
        }

        if (tokens[at].span.begin <= offset && offset < tokens[at].span.end) {
            found = at;
            break;
        }

        if (tokens[at].span.end == offset) {
            found = at;
        }
    }

    if (found == NoToken) {
        return false;
    }

    bool hit = false;
    auto visit = [&](const LspNamed& candidate) noexcept {
        if (!hit && candidate.token == found) {
            named = candidate;
            hit = true;
        }

        return Result<void>::Success();
    };
    for (uint32 node = 0; node < tree->NodeCount() && !hit; ++node) {
        static_cast<void>(LspVisitNames(compilation, module, node, visit));
    }

    return hit;
}

Result<void> LspAppendDeclaration(const Compilation& compilation, uint32 symbolIndex,
                                  StringBuilderBase& out) noexcept {
    const Symbol& symbol = compilation.Symbols()[symbolIndex];
    switch (symbol.kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType: {
        out.Append("type ").Append(symbol.name);
        bool first = true;
        for (const Symbol& parameter : compilation.Symbols()) {
            if (parameter.kind == SymbolKind::LanguageTypeParameter &&
                parameter.owner == symbolIndex) {
                out.Append(first ? "<" : ", ").Append(parameter.name);
                first = false;
            }
        }

        out.Append(first ? "" : ">");
        break;
    }
    case SymbolKind::PreludeValue:
        if (symbol.signature != NoSignature) {
            out.Append(LspIsAsync(compilation, symbol) ? "async fn " : "fn ").Append(symbol.name);
            LspAppendParameters(compilation, symbolIndex, out);
            LspAppendReturn(compilation, symbol, out);
        } else {
            out.Append(symbol.name);
        }

        break;
    case SymbolKind::ContextName:
        out.Append(symbol.name).Append(": ");
        LspAppendType(compilation, symbol.type, out);
        break;
    case SymbolKind::RpcPrefix:
        out.Append(symbol.name);
        break;
    case SymbolKind::EngineType: {
        StringView keyword = "class ";
        if (compilation.SchemaIsValid(symbol.index) &&
            symbol.item < compilation.Schema(symbol.index).Types().Size()) {
            switch (compilation.Schema(symbol.index).Types()[symbol.item].kind) {
            case SchemaTypeKind::Struct:
                keyword = "struct ";
                break;
            case SchemaTypeKind::Enum:
                keyword = "enum ";
                break;
            case SchemaTypeKind::Interface:
                keyword = "interface ";
                break;
            default:
                break;
            }
        }

        out.Append(keyword).Append(LspEnginePrefix).Append(symbol.name);
        if (symbol.base != NoSymbol) {
            out.Append(" : ")
                .Append(LspEnginePrefix)
                .Append(compilation.Symbols()[symbol.base].name);
        }

        break;
    }
    case SymbolKind::Class:
        out.Append("class ").Append(symbol.name);
        for (uint32 at = 0; at < symbol.baseCount; ++at) {
            const uint32 base = compilation.SymbolBases()[symbol.firstBase + at];
            out.Append(at == 0 ? " : " : ", ").Append(compilation.Symbols()[base].name);
        }

        break;
    case SymbolKind::Struct:
        out.Append("struct ").Append(symbol.name);
        break;
    case SymbolKind::Enum:
        out.Append("enum ").Append(symbol.name);
        break;
    case SymbolKind::Interface:
        out.Append("interface ").Append(symbol.name);
        break;
    case SymbolKind::Field:
    case SymbolKind::EngineField:
    case SymbolKind::LanguageField: {
        const bool readonly = LspIsReadOnly(compilation, symbol);
        out.Append(readonly ? "let " : "var ");
        LspAppendOwner(compilation, symbol, out);
        out.Append(symbol.name).Append(": ");
        LspAppendType(compilation, symbol.type, out);
        break;
    }
    case SymbolKind::Const:
        out.Append("const ").Append(symbol.name).Append(": ");
        LspAppendType(compilation, symbol.type, out);
        break;
    case SymbolKind::Function:
    case SymbolKind::Method:
    case SymbolKind::InterfaceMethod:
    case SymbolKind::EngineFunction:
    case SymbolKind::LanguageFunction: {
        out.Append(LspIsAsync(compilation, symbol) ? "async fn " : "fn ");
        LspAppendOwner(compilation, symbol, out);
        out.Append(symbol.name);
        LspAppendParameters(compilation, symbolIndex, out);
        LspAppendReturn(compilation, symbol, out);
        break;
    }
    case SymbolKind::Event:
    case SymbolKind::EngineEvent:
    case SymbolKind::LanguageEvent:
        out.Append("event ");
        LspAppendOwner(compilation, symbol, out);
        out.Append(symbol.name);
        LspAppendParameters(compilation, symbolIndex, out);
        break;
    case SymbolKind::EnumCase:
        LspAppendOwner(compilation, symbol, out);
        out.Append(symbol.name);
        break;
    case SymbolKind::Parameter:
        out.Append("param ").Append(symbol.name).Append(": ");
        LspAppendType(compilation, symbol.type, out);
        break;
    case SymbolKind::Local: {
        out.Append((symbol.flags & SymbolFlags::Let) != 0 ? "let " : "var ").Append(symbol.name);
        const uint32 type = symbol.type;
        if (type != NoType) {
            out.Append(": ");
            LspAppendType(compilation, type, out);
        }

        break;
    }
    case SymbolKind::Alias:
        out.Append("import ").Append(symbol.index < compilation.ModuleCount()
                                         ? compilation.ModuleImportPath(symbol.index)
                                         : symbol.name);
        break;
    case SymbolKind::EngineTypeParameter:
    case SymbolKind::LanguageTypeParameter:
        out.Append("type parameter ").Append(symbol.name);
        break;
    }

    return out.Status();
}

Result<bool> LspAppendHover(const Compilation& compilation, const SourceSet& sources,
                            const LspNamed& named, StringBuilderBase& out) noexcept {
    StringBuilder<256> declaration(out.GetAllocator());
    if (named.symbol != NoSymbol) {
        if (auto appended = LspAppendDeclaration(compilation, named.symbol, declaration);
            !appended) {
            return Result<bool>::Failure(appended.Error());
        }
    } else if (named.import && named.module != NoModule) {
        declaration.Append("import ").Append(compilation.ModuleImportPath(named.module));
    } else {
        return Result<bool>::Success(false);
    }

    out.Append("```sparks\n").Append(declaration.View()).Append("\n```");
    if (named.symbol != NoSymbol) {
        StringBuilder<256> doc(out.GetAllocator());
        if (LspAppendDoc(compilation, sources, compilation.Symbols()[named.symbol], doc)) {
            out.Append("\n\n").Append(doc.View());
        }

        if (doc.HasFailed()) {
            return Result<bool>::Failure(doc.Status().Error());
        }
    }

    // 선언 위치: 사용자 모듈은 `패키지/모듈 경로:줄`, schema는 파일 이름과 줄(기기마다 다른 절대
    // 경로를 보이지 않는다).
    SourceId source;
    SourceSpan span;
    if (LspDefinition(compilation, named, source, span)) {
        const uint32 line = sources.Get(source).Locate(span.begin).line;
        uint32 owner = NoModule;
        for (uint32 module = 0; module < compilation.ModuleCount(); ++module) {
            owner = compilation.ModuleSource(module) == source ? module : owner;
        }

        bool schema = false;
        for (uint32 index = 0; index < compilation.SchemaCount(); ++index) {
            schema = schema || (compilation.SchemaIsValid(index) &&
                                compilation.Schema(index).Source() == source);
        }

        if (owner != NoModule && compilation.PackageIsValid(compilation.ModulePackage(owner))) {
            out.AppendFormat("\n\nDeclared at `{}/{}:{}`.",
                             compilation.Manifest(compilation.ModulePackage(owner)).Name(),
                             compilation.ModulePath(owner), line);
        } else if (schema) {
            const StringView path = sources.Get(source).Path();
            usize slash = path.ByteCount();
            while (slash > 0 && path.Data()[slash - 1] != '/' && path.Data()[slash - 1] != '\\') {
                --slash;
            }

            out.AppendFormat("\n\nDeclared in the engine schema at `{}:{}`.",
                             StringView(path.Data() + slash, path.ByteCount() - slash), line);
        }
    }

    if (out.HasFailed() || declaration.HasFailed()) {
        return Result<bool>::Failure(out.HasFailed() ? out.Status().Error()
                                                     : declaration.Status().Error());
    }

    return Result<bool>::Success(true);
}

bool LspDefinition(const Compilation& compilation, const LspNamed& named, SourceId& source,
                   SourceSpan& span) noexcept {
    if (named.symbol == NoSymbol) {
        if (!named.import || named.module == NoModule ||
            named.module >= compilation.ModuleCount()) {
            return false;
        }

        source = compilation.ModuleSource(named.module);
        span = SourceSpan{};
        return true;
    }

    const Symbol& symbol = compilation.Symbols()[named.symbol];
    if (symbol.kind == SymbolKind::Alias) {
        if (symbol.index >= compilation.ModuleCount()) {
            return false;
        }

        source = compilation.ModuleSource(symbol.index);
        span = SourceSpan{};
        return true;
    }

    if (symbol.module != NoModule) {
        const SyntaxTree* tree = compilation.ModuleTree(symbol.module);
        if (tree == nullptr || symbol.nameNode >= tree->NodeCount() ||
            tree->Node(symbol.nameNode).token == NoToken) {
            return false;
        }

        source = tree->Source();
        span = tree->Tokens()[tree->Node(symbol.nameNode).token].span;
        return true;
    }

    // 엔진 심볼: schema 파일의 그 항목 `name` 값.
    uint32 schemaIndex = symbol.index;
    bool engineCase = false;
    if (symbol.kind == SymbolKind::EnumCase) {
        engineCase = symbol.owner != NoSymbol &&
                     compilation.Symbols()[symbol.owner].kind == SymbolKind::EngineType;
        schemaIndex = engineCase ? compilation.Symbols()[symbol.owner].index : schemaIndex;
    }

    if ((!LspIsEngine(symbol.kind) && !engineCase) || !compilation.SchemaIsValid(schemaIndex)) {
        return false;
    }

    const BindingSchema& schema = compilation.Schema(schemaIndex);
    const uint32 item = symbol.item;
    bool found = false;
    switch (symbol.kind) {
    case SymbolKind::EngineType:
        found = item < schema.Types().Size();
        span = found ? schema.Types()[item].span : span;
        break;
    case SymbolKind::EngineField:
        found = item < schema.Fields().Size();
        span = found ? schema.Fields()[item].span : span;
        break;
    case SymbolKind::EngineFunction:
        found = item < schema.Functions().Size();
        span = found ? schema.Functions()[item].span : span;
        break;
    case SymbolKind::EngineEvent:
        found = item < schema.Events().Size();
        span = found ? schema.Events()[item].span : span;
        break;
    case SymbolKind::EnumCase:
        found = item < schema.Cases().Size();
        span = found ? schema.Cases()[item].span : span;
        break;
    default:
        break;
    }

    source = schema.Source();
    return found;
}

} // namespace rocks::sparks::tools::detail
