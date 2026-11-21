#include "CompilationChecker.h"
#include "TypeChecker.h"

// 권한·RPC·저장 타입 규칙이다(Docs/Design/SparksLanguage.ko.md 3.15절): 저장 가능 타입(S1001), 복제
// 타입(S1002), RPC 호출(S1004). 권한 구역 밖의 `@net` 쓰기(S1003)는 효과 분석(CheckContexts.cpp)이
// 본다. Rules 단계(LC3)에서만 돈다.

namespace rocks::sparks::detail {

namespace {

// 저장·복제 타입 판정의 깊이 상한이다(값 struct 순환은 S0426이 막는다).
constexpr uint32 MaxStorableDepth = 32;

constexpr StringView StorablePreludeTypes[] = {"Rotator", "Color", "Transform", "Tag",
                                               "TagSet",  "Soft",  "Row",       "Clock"};
constexpr StringView StorableContainers[] = {"Array", "Map", "Set"};

} // namespace

bool TypeChecker::IsStorableType(uint32 type, bool objects, uint32 depth) noexcept {
    const Compilation& c = compilation_;
    if (type == NoType || depth > MaxStorableDepth) {
        return false;
    }

    const uint32 value = NonOptional(type);
    const uint32 symbol = SymbolOf(value);
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = c.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::BuiltinType:
        return value != voidType_;
    case SymbolKind::PreludeType: {
        for (const StringView name : StorablePreludeTypes) {
            if (entry.name == name) {
                return true;
            }
        }

        for (const StringView name : StorableContainers) {
            if (entry.name != name) {
                continue;
            }

            const TypeEntry& shape = c.types_[value];
            for (uint32 index = 0; index < shape.argumentCount; ++index) {
                if (!IsStorableType(c.typeArguments_[shape.firstArgument + index], objects,
                                    depth + 1)) {
                    return false;
                }
            }

            return true;
        }

        return false; // Text·Result·LoadRequest·InputAction 등
    }
    case SymbolKind::Enum:
        return true;
    case SymbolKind::Class:
    case SymbolKind::Interface:
        return objects;
    case SymbolKind::Struct: {
        const SyntaxTree& tree = c.trees_[entry.module];
        const uint32 members = tree.Child(entry.node, 3);
        if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
            return true;
        }

        for (const uint32 member : tree.Children(members)) {
            if (tree.Node(member).kind != SyntaxKind::Field) {
                continue;
            }

            const uint32 field = c.NodeSymbol(entry.module, tree.Child(member, 2));
            if (field != NoSymbol && !IsStorableType(FieldType(field), objects, depth + 1)) {
                return false;
            }
        }

        return true;
    }
    case SymbolKind::EngineType: {
        const SchemaTypeKind kind = c.schemas_[entry.index].schema.Types()[entry.item].kind;
        if (kind == SchemaTypeKind::Enum) {
            return true;
        }

        if (kind == SchemaTypeKind::Object || kind == SchemaTypeKind::Interface) {
            return objects && !c.schemas_[entry.index].schema.Types()[entry.item].held;
        }

        if (kind != SchemaTypeKind::Struct) {
            return false; // 로드한 에셋 값
        }

        for (uint32 id = 0; id < c.symbols_.Size(); ++id) {
            const Symbol& field = c.symbols_[id];
            if (field.kind == SymbolKind::EngineField && field.owner == symbol &&
                !IsStorableType(field.type, objects, depth + 1)) {
                return false;
            }
        }

        return true;
    }
    default:
        return false;
    }
}

StringView TypeChecker::RpcTarget(uint32 function) const noexcept {
    const Compilation& c = compilation_;
    const Symbol& entry = c.symbols_[function];
    if (entry.kind != SymbolKind::Method || entry.module >= c.modules_.Size()) {
        return StringView();
    }

    const SyntaxTree& tree = c.trees_[entry.module];
    const uint32 attributes = tree.Child(entry.node, 1);
    if (attributes == NoNode || tree.Node(attributes).kind != SyntaxKind::List) {
        return StringView();
    }

    for (const uint32 attribute : tree.Children(attributes)) {
        if (tree.Node(attribute).kind != SyntaxKind::Attribute ||
            TokenText(entry.module, attribute) != "rpc") {
            continue;
        }

        const uint32 list = tree.Child(attribute, 0);
        if (list == NoNode || tree.Node(list).childCount == 0) {
            return "?"; // 대상이 없는 `@rpc`(S0623)
        }

        const uint32 value = tree.Child(tree.Child(list, 0), 1);
        return value != NoNode && tree.Node(value).kind == SyntaxKind::Identifier
                   ? TokenText(entry.module, value)
                   : StringView("?");
    }

    return StringView();
}

ExpressionValue TypeChecker::RpcMember(const CheckContext& context, uint32 node, uint32 prefix,
                                       StringView name) noexcept {
    const Compilation& c = compilation_;
    const StringView target = c.symbols_[prefix].name;
    ExpressionValue result;
    const uint32 member =
        context.thisType == NoType ? NoSymbol : FindMember(context.thisType, name);
    auto misuse = [&](StringView message, StringView fix) noexcept {
        Report(context, DiagnosticCode::RpcCall, node, message,
               "'Server.', 'AllClients.' and 'OwnerClient.' call an RPC function of this object.",
               fix);
    };
    if (member == NoSymbol || c.symbols_[member].kind != SymbolKind::Method) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is not a function of this class.", name);
        if (binder_.Succeeded(message)) {
            misuse(message.View(), "Call an '@rpc' function of this class.");
        }

        return result;
    }

    const StringView declared = RpcTarget(member);
    if (declared.IsEmpty()) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is not an RPC.", name);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        fix.AppendFormat("Call it directly as '{}(...)', or declare it '@rpc({})'.", name, target);
        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            misuse(message.View(), fix.View());
        }

        return result;
    }

    if (declared != target && declared != "?") {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is an RPC to '{}'.", name, declared);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        fix.AppendFormat("Write '{}.{}(...)'.", declared, name);
        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            misuse(message.View(), fix.View());
        }

        return result;
    }

    binder_.SetNodeSymbol(context.module, node, member);
    return ValueOfSymbol(context, member, context.thisType, true);
}

void TypeChecker::CheckNetwork(uint32 module) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;
    auto typeText = [&](StringBuilderBase& out, uint32 type) noexcept {
        AppendDisplayType(out, type);
    };
    for (uint32 node = 0; node < tree.NodeCount() && binder_.error_ == ErrorCode::None; ++node) {
        const SyntaxNode& entry = tree.Node(node);
        if (entry.kind == SyntaxKind::Field) {
            const bool save = HasAttribute(module, node, "save");
            const bool net = HasAttribute(module, node, "net");
            if (!save && !net) {
                continue;
            }

            const uint32 field = c.NodeSymbol(module, tree.Child(node, 2));
            const uint32 type = field == NoSymbol ? NoType : FieldType(field);
            if (type == NoType || IsHeldType(type)) {
                continue; // 유지 자원은 S0904가 알린다
            }

            const uint32 at =
                tree.Child(node, 3) != NoNode ? tree.Child(node, 3) : tree.Child(node, 2);
            if (save && !IsStorableType(type, false, 0)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("A '@save' field cannot hold '");
                typeText(message, type);
                message.Append("'.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::NotSaveable, at, message.View(),
                           "Saved values are plain data: numbers, strings, enums, tags, 'Soft' and "
                           "'Row' references, and structs and containers of them.",
                           "Save a 'Soft<T>' reference or a value instead.");
                }
            } else if (net && !IsStorableType(type, true, 0)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("A '@net' field cannot hold '");
                typeText(message, type);
                message.Append("'.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::NotReplicable, at, message.View(),
                           "Replicated values are saveable values and object references; loaded "
                           "assets are replicated as 'Soft<T>'.",
                           "Replicate a 'Soft<T>' reference or a value instead.");
                }
            }

            continue;
        }

        if (entry.kind != SyntaxKind::Function) {
            continue;
        }

        const uint32 function = c.NodeSymbol(module, tree.Child(node, 2));
        if (function == NoSymbol || RpcTarget(function).IsEmpty()) {
            continue;
        }

        const uint32 parameters = tree.Child(node, 3);
        if (parameters != NoNode && tree.Node(parameters).kind == SyntaxKind::List) {
            for (const uint32 parameter : tree.Children(parameters)) {
                const uint32 typeNode = tree.Child(parameter, 1);
                const uint32 type = typeNode == NoNode ? NoType : c.NodeType(module, typeNode);
                if (type == NoType || IsHeldType(type) || IsStorableType(type, true, 0)) {
                    continue;
                }

                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("An RPC argument cannot be '");
                typeText(message, type);
                message.Append("'.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::NotReplicable, typeNode, message.View(),
                           "RPC arguments are sent over the network as saveable values and "
                           "object references.",
                           "Pass a 'Soft<T>' reference or a value instead.");
                }
            }
        }

        const uint32 returns = tree.Child(node, 4);
        if (returns != NoNode) {
            Report(context, DiagnosticCode::NotReplicable, returns, "An RPC returns nothing.",
                   "An RPC is sent and runs elsewhere, so the caller never gets a result.",
                   "Remove the return type, and send the answer back with another RPC.");
        }
    }
}

} // namespace rocks::sparks::detail
