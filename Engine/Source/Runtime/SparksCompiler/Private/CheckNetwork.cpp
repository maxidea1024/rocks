#include "CompilationChecker.h"
#include "TypeChecker.h"

// 권한·RPC·저장 타입 규칙이다(Docs/Design/SparksLanguage 3.15절): 저장 가능 타입(S1001), 복제
// 타입(S1002), RPC 호출(S1004). 권한 구역 밖의 `@net` 쓰기(S1003)는 효과 분석(CheckContexts.cpp)이
// 본다. Rules 단계(LC3)에서만 돈다.

namespace rocks::sparks::detail {

namespace {

// Class<T>는 클래스 경로로 저장·복제한다(UE TSubclassOf). mat4·Clock은 숫자 묶음이다.
constexpr StringView StorablePreludeTypes[] = {"Rotator", "Color", "Transform", "Tag",   "TagSet",
                                               "Soft",    "Row",   "Clock",     "Class", "mat4"};
constexpr StringView StorableContainers[] = {"Array", "Map", "Set"};

// 타입별 판정 기억 표의 상태다(타입마다 객체 허용 여부 둘).
enum StorableState : uint8 {
    StorableUnknown = 0,
    StorableComputing = 1, // 계산 중 — 컨테이너를 거친 자기 참조는 가능으로 본다
    StorableYes = 2,
    StorableNo = 3
};

constexpr uint32 StorableStatesPerType = 2;

} // namespace

bool TypeChecker::IsStorableType(uint32 type, bool objects) noexcept {
    const Compilation& c = compilation_;
    if (type == NoType) {
        return false;
    }

    const uint32 value = NonOptional(type);
    const usize slot = usize(value) * StorableStatesPerType + (objects ? 1 : 0);
    if (slot >= storable_.Size()) {
        const usize size = usize(c.types_.Size()) * StorableStatesPerType;
        if (auto resized = storable_.TryResize(size > slot ? size : slot + 1); !resized) {
            binder_.error_ = resized.Error();
            return true; // 오류는 binder에 — 진단을 더 내지 않는다
        }
    }

    if (storable_[slot] != StorableUnknown) {
        return storable_[slot] != StorableNo;
    }

    storable_[slot] = StorableComputing;
    const bool result = ComputeStorable(value, objects);
    storable_[slot] = result ? StorableYes : StorableNo;
    return result;
}

bool TypeChecker::ComputeStorable(uint32 value, bool objects) noexcept {
    const Compilation& c = compilation_;
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
                if (!IsStorableType(c.typeArguments_[shape.firstArgument + index], objects)) {
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
            if (field != NoSymbol && !IsStorableType(FieldType(field), objects)) {
                return false;
            }
        }

        return true;
    }
    case SymbolKind::EngineType: {
        const BindingSchema& schema = c.schemas_[entry.index].schema;
        const SchemaType& shape = schema.Types()[entry.item];
        if (shape.kind == SchemaTypeKind::Enum) {
            return true;
        }

        if (shape.kind == SchemaTypeKind::Object || shape.kind == SchemaTypeKind::Interface) {
            return objects && !shape.held;
        }

        if (shape.kind != SchemaTypeKind::Struct) {
            return false; // 로드한 에셋 값
        }

        // schema의 필드 구간을 이름으로 찾는다(심볼 표 전체를 훑지 않는다).
        for (uint32 index = shape.firstField; index < shape.firstField + shape.fieldCount;
             ++index) {
            const uint32 field = FindMember(value, schema.Text(schema.Fields()[index].name));
            if (field != NoSymbol && !IsStorableType(c.symbols_[field].type, objects)) {
                return false;
            }
        }

        return true;
    }
    default:
        return false;
    }
}

bool TypeChecker::ContainsHeldType(uint32 type) noexcept {
    // 타입 인자 안까지 본다(타입 인자의 중첩은 원문 깊이로 유한하다).
    const Compilation& c = compilation_;
    if (type == NoType) {
        return false;
    }

    const uint32 value = NonOptional(type);
    if (IsHeldType(value)) {
        return true;
    }

    const TypeEntry& shape = c.types_[value];
    for (uint32 index = 0; index < shape.argumentCount; ++index) {
        if (ContainsHeldType(c.typeArguments_[shape.firstArgument + index])) {
            return true;
        }
    }

    return false;
}

uint32 TypeChecker::OverriddenMember(uint32 function) const noexcept {
    const Compilation& c = compilation_;
    if (function == NoSymbol || (c.symbols_[function].flags & SymbolFlags::Override) == 0) {
        return NoSymbol;
    }

    const uint32 owner = c.symbols_[function].owner;
    const uint32 base = owner == NoSymbol ? NoSymbol : c.symbols_[owner].base;
    return base == NoSymbol ? NoSymbol : binder_.FindMemberInChain(base, c.symbols_[function].name);
}

bool TypeChecker::OverridesEngineFunction(uint32 function) const noexcept {
    const Compilation& c = compilation_;
    for (uint32 step = 0; step < c.symbols_.Size() && function != NoSymbol; ++step) {
        function = OverriddenMember(function);
        if (function != NoSymbol && c.symbols_[function].kind == SymbolKind::EngineFunction) {
            return true;
        }
    }

    return false;
}

StringView TypeChecker::RpcTarget(uint32 function) const noexcept {
    // 자기 `@rpc`, 없으면 재정의한 기반 함수 사슬의 것(UE처럼 재정의는 RPC 구현만 바꾼다).
    const Compilation& c = compilation_;
    for (uint32 step = 0; step < c.symbols_.Size() && function != NoSymbol; ++step) {
        const Symbol& entry = c.symbols_[function];
        if (entry.kind != SymbolKind::Method || entry.module >= c.modules_.Size()) {
            return StringView();
        }

        const SyntaxTree& tree = c.trees_[entry.module];
        const uint32 attributes = tree.Child(entry.node, 1);
        if (attributes != NoNode && tree.Node(attributes).kind == SyntaxKind::List) {
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
        }

        function = OverriddenMember(function);
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
            if (type == NoType || ContainsHeldType(type)) {
                continue; // 유지 자원은 S0904가 알린다
            }

            const uint32 at =
                tree.Child(node, 3) != NoNode ? tree.Child(node, 3) : tree.Child(node, 2);
            if (save && !IsStorableType(type, false)) {
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
            } else if (net && !IsStorableType(type, true)) {
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
                if (type == NoType || ContainsHeldType(type) || IsStorableType(type, true)) {
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
        if (returns != NoNode && c.NodeType(module, returns) != voidType_) {
            Report(context, DiagnosticCode::NotReplicable, returns, "An RPC returns nothing.",
                   "An RPC is sent and runs elsewhere, so the caller never gets a result.",
                   "Remove the return type, and send the answer back with another RPC.");
        }
    }
}

} // namespace rocks::sparks::detail
