#include <Core/Algorithms.h>
#include <SparksCompiler/Compilation.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"

// 타입 정규 표기와 이름 해석 덤프다(Docs/Design/SparksLanguage.ko.md 3.9절 "이름 해석 덤프", SD58).
// 형식은 Compilation.h의 AppendTypeText·AppendNameDump 설명이다.

namespace rocks::sparks {

namespace {

// 덤프 한 줄의 종류다(같은 위치에서 선언 → 참조 → 타입 순).
enum class DumpLineKind : uint8 {
    Declaration,
    Reference,
    Type
};

struct DumpLine {
    uint32 begin = 0;
    uint32 node = 0;
    DumpLineKind kind = DumpLineKind::Declaration;
};

StringView KindWord(SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::BuiltinType:
        return "builtin";
    case SymbolKind::PreludeType:
    case SymbolKind::PreludeValue:
        return "prelude";
    case SymbolKind::ContextName:
        return "context";
    case SymbolKind::RpcPrefix:
        return "rpc";
    case SymbolKind::EngineType:
        return "engine-type";
    case SymbolKind::EngineField:
        return "engine-field";
    case SymbolKind::EngineFunction:
        return "engine-fn";
    case SymbolKind::EngineEvent:
        return "engine-event";
    case SymbolKind::EngineTypeParameter:
        return "type-param";
    case SymbolKind::Alias:
        return "alias";
    case SymbolKind::Class:
        return "class";
    case SymbolKind::Struct:
        return "struct";
    case SymbolKind::Enum:
        return "enum";
    case SymbolKind::Interface:
        return "interface";
    case SymbolKind::Const:
        return "const";
    case SymbolKind::Function:
        return "fn";
    case SymbolKind::Field:
        return "field";
    case SymbolKind::Method:
        return "method";
    case SymbolKind::Event:
        return "event";
    case SymbolKind::EnumCase:
        return "case";
    case SymbolKind::InterfaceMethod:
        return "imethod";
    case SymbolKind::Parameter:
        return "param";
    case SymbolKind::Local:
        return "local";
    case SymbolKind::LanguageField:
        return "lang-field";
    case SymbolKind::LanguageFunction:
        return "lang-fn";
    case SymbolKind::LanguageEvent:
        return "lang-event";
    case SymbolKind::LanguageTypeParameter:
        return "type-param";
    }

    return "?";
}

} // namespace

Result<void> Compilation::AppendSymbolName(StringBuilderBase& out, uint32 symbol) const noexcept {
    const Symbol& entry = symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::EngineType:
        out.Append(EnginePackageName).Append(".").Append(entry.name);
        break;
    case SymbolKind::LanguageField:
    case SymbolKind::LanguageFunction:
    case SymbolKind::LanguageEvent:
        if (entry.owner != NoSymbol && entry.owner != symbol) {
            out.Append(symbols_[entry.owner].name).Append(".");
        }

        out.Append(entry.name);
        break;
    case SymbolKind::EngineField:
    case SymbolKind::EngineFunction:
    case SymbolKind::EngineEvent:
        out.Append(EnginePackageName)
            .Append(".")
            .Append(symbols_[entry.owner].name)
            .Append(".")
            .Append(entry.name);
        break;
    case SymbolKind::EnumCase:
        if (entry.module == NoModule && symbols_[entry.owner].kind != SymbolKind::EngineType) {
            out.Append(symbols_[entry.owner].name).Append(".").Append(entry.name);
            break;
        }

        if (entry.module == NoModule) {
            out.Append(EnginePackageName)
                .Append(".")
                .Append(symbols_[entry.owner].name)
                .Append(".")
                .Append(entry.name);
            break;
        }

        [[fallthrough]];
    case SymbolKind::Field:
    case SymbolKind::Method:
    case SymbolKind::Event:
    case SymbolKind::InterfaceMethod:
        if (auto owner = AppendSymbolName(out, entry.owner); !owner) {
            return owner;
        }

        out.Append(".").Append(entry.name);
        break;
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
    case SymbolKind::Const:
    case SymbolKind::Function: {
        const ModuleEntry& module = modules_[entry.module];
        out.Append(packages_[module.package].manifest.Name())
            .Append(":")
            .Append(Text(module.importPath))
            .Append(".")
            .Append(entry.name);
        break;
    }
    case SymbolKind::Parameter:
    case SymbolKind::Local: {
        const SourceFile& file = sources_->Get(modules_[entry.module].source);
        const SourceLocation at = file.Locate(trees_[entry.module].Node(entry.nameNode).span.begin);
        out.AppendFormat("{}@{}:{}", entry.name, at.line, at.column);
        break;
    }
    case SymbolKind::Alias: {
        const ModuleEntry& target = modules_[entry.index];
        out.Append(entry.name)
            .Append("=")
            .Append(packages_[target.package].manifest.Name())
            .Append(":")
            .Append(Text(target.importPath));
        break;
    }
    case SymbolKind::BuiltinType: {
        const StringView canonical = detail::LanguageNames()[entry.index].canonical;
        out.Append(canonical.IsEmpty() ? entry.name : canonical);
        break;
    }
    default:
        out.Append(entry.name);
        break;
    }

    return out.Status();
}

Result<void> Compilation::AppendTypeText(StringBuilderBase& out, uint32 type) const noexcept {
    if (type == NoType || type >= types_.Size()) {
        out.Append("?");
        return out.Status();
    }

    const TypeEntry& entry = types_[type];
    if (auto name = AppendSymbolName(out, entry.symbol); !name) {
        return name;
    }

    if (entry.argumentCount != 0) {
        out.Append("<");
        for (uint32 index = 0; index < entry.argumentCount; ++index) {
            if (index != 0) {
                out.Append(", ");
            }

            if (auto argument = AppendTypeText(out, typeArguments_[entry.firstArgument + index]);
                !argument) {
                return argument;
            }
        }

        out.Append(">");
    }

    if (entry.optional) {
        out.Append("?");
    }

    return out.Status();
}

Result<void> Compilation::AppendNameDump(StringBuilderBase& out, uint32 module) const noexcept {
    if (module >= modules_.Size() || !modules_[module].parsed || module >= nodeBase_.Size() ||
        nodeBase_[module] == NoNode) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SyntaxTree& tree = trees_[module];
    const uint32 base = nodeBase_[module];
    Array<DumpLine> lines(*allocator_);
    for (uint32 node = 0; node < tree.NodeCount(); ++node) {
        const uint32 symbol = nodeSymbols_[base + node];
        const uint32 begin = tree.Node(node).span.begin;
        if (symbol != detail::NotRecorded) {
            const bool declaration = symbol != NoSymbol && symbols_[symbol].module == module &&
                                     symbols_[symbol].nameNode == node;
            if (auto pushed = lines.TryPushBack(
                    DumpLine{begin, node,
                             declaration ? DumpLineKind::Declaration : DumpLineKind::Reference});
                !pushed) {
                return pushed;
            }
        }

        if (nodeTypes_[base + node] != detail::NotRecorded) {
            if (auto pushed = lines.TryPushBack(DumpLine{begin, node, DumpLineKind::Type});
                !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(lines.View(), [](const DumpLine& left, const DumpLine& right) noexcept {
        if (left.begin != right.begin) {
            return left.begin < right.begin;
        }

        if (left.node != right.node) {
            return left.node < right.node;
        }

        return static_cast<uint8>(left.kind) < static_cast<uint8>(right.kind);
    });
    const SourceFile& file = sources_->Get(modules_[module].source);
    const StringView text = file.Text();
    for (const DumpLine& line : lines) {
        const SourceLocation at = file.Locate(line.begin);
        out.AppendFormat("{}:{} ", at.line, at.column);
        const uint32 symbol = nodeSymbols_[base + line.node];
        switch (line.kind) {
        case DumpLineKind::Declaration:
            out.Append("decl ").Append(KindWord(symbols_[symbol].kind)).Append(" ");
            if (auto name = AppendSymbolName(out, symbol); !name) {
                return name;
            }

            break;
        case DumpLineKind::Reference: {
            const SyntaxNode& node = tree.Node(line.node);
            const SourceSpan span = tree.Tokens()[node.token].span;
            out.Append(StringView(text.Data() + span.begin, span.end - span.begin)).Append(" -> ");
            if (symbol == NoSymbol) {
                out.Append("?");
            } else {
                out.Append(KindWord(symbols_[symbol].kind)).Append(" ");
                if (auto name = AppendSymbolName(out, symbol); !name) {
                    return name;
                }
            }

            break;
        }
        case DumpLineKind::Type:
            out.Append("type ");
            if (auto type = AppendTypeText(out, nodeTypes_[base + line.node]); !type) {
                return type;
            }

            break;
        }

        out.Append("\n");
    }

    return out.Status();
}

} // namespace rocks::sparks
