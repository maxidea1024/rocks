#include "CompilationChecker.h"
#include "TypeChecker.h"

// 유지 자원·로드 규칙이다(Docs/Design/SparksLanguage.ko.md 3.14절): 버림(S0901), 담는 자리(S0902),
// 옮김(S0903), 담는 선언(S0904), 기다리지 않는 지역 자원(S0905). Rules 단계(LC3)에서만 돈다.

namespace rocks::sparks::detail {

bool TypeChecker::IsHeldType(uint32 type) noexcept {
    if (type == NoType) {
        return false;
    }

    const uint32 value = NonOptional(type);
    if (IsLanguage(value, "LoadRequest")) {
        return true;
    }

    const uint32 symbol = SymbolOf(value);
    if (symbol == NoSymbol || compilation_.symbols_[symbol].kind != SymbolKind::EngineType) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    return compilation_.schemas_[entry.index].schema.Types()[entry.item].held;
}

void TypeChecker::CheckResources(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;
    const uint32 count = tree.NodeCount();
    // 부모 표(노드 → 부모)다.
    Array<uint32> parent(allocator_);
    if (auto resized = parent.TryResize(count); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32& entry : parent) {
        entry = NoNode;
    }

    for (uint32 node = 0; node < count; ++node) {
        for (const uint32 child : tree.Children(node)) {
            if (child != NoNode && child < count) {
                parent[child] = node;
            }
        }
    }

    auto effectiveParent = [&](uint32 node, uint32& self) noexcept {
        self = node;
        uint32 up = parent[node];
        while (up != NoNode && tree.Node(up).kind == SyntaxKind::Parenthesized) {
            self = up;
            up = parent[up];
        }

        return up;
    };
    auto isWaitArgument = [&](uint32 argument) noexcept {
        // `All`·`Race`의 인자(Argument → List → Call)다.
        const uint32 list = parent[argument];
        const uint32 call = list == NoNode ? NoNode : parent[list];
        if (call == NoNode || tree.Node(call).kind != SyntaxKind::Call) {
            return false;
        }

        const uint32 symbol = c.NodeSymbol(module, tree.Child(call, 0));
        return symbol != NoSymbol && (c.symbols_[symbol].flags & SymbolFlags::Variadic) != 0;
    };
    auto isAwaitOperand = [&](uint32 up) noexcept {
        const SyntaxNode& entry = tree.Node(up);
        return entry.kind == SyntaxKind::Await ||
               (entry.kind == SyntaxKind::Try && (entry.flags & SyntaxFlags::Await) != 0);
    };
    auto reportType = [&](uint32 typeNode, StringView message) noexcept {
        Report(context, DiagnosticCode::ResourceDeclaration, typeNode, message,
               "A held resource stops when its variable ends, so it lives only in a local name or "
               "a 'var' field of type 'T?'.",
               "Keep it in a local variable or a 'var' field declared 'T?'.");
    };

    for (uint32 node = 0; node < count && binder_.error_ == ErrorCode::None; ++node) {
        const SyntaxNode& entry = tree.Node(node);
        // 타입 인자 자리의 유지 자원 타입(S0904).
        if (entry.kind == SyntaxKind::Type) {
            const uint32 list = tree.Child(node, 1);
            if (list != NoNode && tree.Node(list).kind == SyntaxKind::List) {
                for (const uint32 argument : tree.Children(list)) {
                    if (IsHeldType(c.NodeType(module, argument))) {
                        reportType(argument, "A held resource cannot be a type argument.");
                    }
                }
            }

            continue;
        }

        // 선언의 유지 자원 타입(S0904): 필드(class `var` `T?`만), 매개변수, 반환 타입.
        if (entry.kind == SyntaxKind::Field) {
            const uint32 typeNode = tree.Child(node, 3);
            const uint32 field = c.NodeSymbol(module, tree.Child(node, 2));
            const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
            if (!IsHeldType(type)) {
                continue;
            }

            const uint32 owner = parent[parent[node]];
            const bool inClass = owner != NoNode && tree.Node(owner).kind == SyntaxKind::Class;
            const bool attributes = HasAttribute(module, node, "save") ||
                                    HasAttribute(module, node, "edit") ||
                                    HasAttribute(module, node, "net");
            const bool request = IsLanguage(NonOptional(type), "LoadRequest");
            if (!inClass || (entry.flags & SyntaxFlags::Let) != 0 || !IsOptional(type) ||
                attributes || request) {
                reportType(typeNode != NoNode ? typeNode : tree.Child(node, 2),
                           request ? StringView("A load request is kept only in a local variable.")
                                   : StringView("A held resource field is a 'var' field of type "
                                                "'T?' without '@save', '@edit' or '@net'."));
            }

            continue;
        }

        if (entry.kind == SyntaxKind::Parameter) {
            const uint32 typeNode = tree.Child(node, 1);
            if (typeNode != NoNode && IsHeldType(c.NodeType(module, typeNode))) {
                reportType(typeNode, "A held resource cannot be passed as an argument.");
            }

            continue;
        }

        if (entry.kind == SyntaxKind::Function || entry.kind == SyntaxKind::InterfaceMethod) {
            const uint32 returns = tree.Child(node, 4);
            if (returns != NoNode && tree.Node(returns).kind == SyntaxKind::Type &&
                IsHeldType(c.NodeType(module, returns))) {
                reportType(returns, "A held resource cannot be returned.");
            }

            continue;
        }

        // 기다리지 않는 지역 자원(S0905).
        if (entry.kind == SyntaxKind::LocalVariable) {
            const uint32 nameNode = tree.Child(node, 0);
            const uint32 local = nameNode == NoNode ? NoSymbol : c.NodeSymbol(module, nameNode);
            uint32 value = tree.Child(node, 2);
            while (value != NoNode && tree.Node(value).kind == SyntaxKind::Parenthesized) {
                value = tree.Child(value, 0);
            }

            // 획득 호출로 만든 지역만 본다(다른 이름에서 옮긴 것은 S0903이 알린다).
            if (local == NoSymbol || !IsHeldType(c.symbols_[local].type) || value == NoNode ||
                tree.Node(value).kind != SyntaxKind::Call) {
                continue;
            }

            const uint32 block = parent[node];
            bool waits = false;
            bool after = false;
            if (block != NoNode && tree.Node(block).kind == SyntaxKind::Block) {
                for (const uint32 statement : tree.Children(block)) {
                    waits = waits || (after && ContainsAwait(module, statement));
                    after = after || statement == node;
                }
            }

            if (!waits) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("'{}' stops at the end of this block without waiting.",
                                     c.symbols_[local].name);
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::ResourceWithoutWait, nameNode, message.View(),
                           "A local held resource stops when its block ends; without 'await' it "
                           "stops in the same script step.",
                           "Keep it in a 'var' field to hold it, or add 'await Delay(...)' to "
                           "hold it for a while.");
                }
            }

            continue;
        }

        if (entry.kind != SyntaxKind::Call && entry.kind != SyntaxKind::Identifier &&
            entry.kind != SyntaxKind::Member) {
            continue;
        }

        const uint32 type = c.ExpressionType(module, node);
        if (!IsHeldType(type)) {
            continue;
        }

        uint32 self = node;
        const uint32 up = effectiveParent(node, self);
        if (up == NoNode) {
            continue;
        }

        const SyntaxNode& upEntry = tree.Node(up);
        if (entry.kind == SyntaxKind::Call) {
            // 획득(담는 자리 — 규칙 1).
            if (upEntry.kind == SyntaxKind::LocalVariable && tree.Child(up, 2) == self) {
                continue;
            }

            if (isAwaitOperand(up) ||
                (upEntry.kind == SyntaxKind::Argument && isWaitArgument(up))) {
                continue;
            }

            if (upEntry.kind == SyntaxKind::ExpressionStatement ||
                upEntry.kind == SyntaxKind::DiscardAssignment) {
                Report(context, DiagnosticCode::DiscardedResource, node,
                       "This held resource is dropped, so there is no way to stop it.",
                       "A held resource lives in the variable that holds it and stops when that "
                       "variable ends.",
                       "Keep it, as in 'let x = ...;' or a 'var' field, or use a one-shot "
                       "'Play' function.");
                continue;
            }

            if (upEntry.kind == SyntaxKind::Assignment && tree.Child(up, 1) == self) {
                const uint32 target = tree.Child(up, 0);
                const uint32 symbol =
                    target != NoNode && tree.Node(target).kind == SyntaxKind::Identifier
                        ? c.NodeSymbol(module, target)
                        : NoSymbol;
                const SymbolKind kind =
                    symbol == NoSymbol ? SymbolKind::BuiltinType : c.symbols_[symbol].kind;
                const bool request = IsLanguage(NonOptional(type), "LoadRequest");
                const bool varLocal =
                    kind == SymbolKind::Local && (c.symbols_[symbol].flags & SymbolFlags::Let) == 0;
                if (varLocal || (kind == SymbolKind::Field && !request)) {
                    continue;
                }
            }

            Report(context, DiagnosticCode::ResourcePlacement, node,
                   IsLanguage(NonOptional(type), "LoadRequest")
                       ? StringView("A load request is created only into a local variable.")
                       : StringView("A held resource is created only into a variable."),
                   "Where a resource is kept decides when it stops.",
                   "Write 'let x = ...;', or assign it to a 'var' field.");
            continue;
        }

        // 담은 이름의 읽기(규칙 3). 대입의 왼쪽은 읽기가 아니다.
        if (upEntry.kind == SyntaxKind::Assignment && tree.Child(up, 0) == self) {
            continue;
        }

        if (entry.kind == SyntaxKind::Member && upEntry.kind == SyntaxKind::Call &&
            tree.Child(up, 0) == self) {
            continue; // 유지 자원을 돌려주는 메서드를 부르는 자리(위의 Call이 본다)
        }

        const bool receiver = (upEntry.kind == SyntaxKind::Member && tree.Child(up, 0) == self);
        bool nullTest = false;
        if (upEntry.kind == SyntaxKind::Binary) {
            const StringView op = TokenText(module, up);
            const uint32 other = tree.Child(up, 0) == self ? tree.Child(up, 1) : tree.Child(up, 0);
            nullTest = (op == "==" || op == "!=") && other != NoNode &&
                       tree.Node(other).kind == SyntaxKind::NullLiteral;
        }

        if (receiver || nullTest || isAwaitOperand(up) ||
            (upEntry.kind == SyntaxKind::Argument && isWaitArgument(up))) {
            continue;
        }

        Report(context, DiagnosticCode::ResourceMoved, node,
               "A held resource cannot be copied or moved.",
               "It stops when the variable that holds it ends, so another name would keep a "
               "stopped resource.",
               "Use it through its variable, as in 'Hum?.Pause()'.");
    }
}

void TypeChecker::CheckAcquireReturns() noexcept {
    const Compilation& c = compilation_;
    for (uint32 id = 0; id < c.symbols_.Size() && binder_.error_ == ErrorCode::None; ++id) {
        const Symbol& entry = c.symbols_[id];
        if (entry.kind != SymbolKind::EngineFunction || entry.signature == NoSignature) {
            continue;
        }

        const auto& schema = c.schemas_[entry.index];
        const SchemaFunction& function = schema.schema.Functions()[entry.item];
        if ((function.effects & SchemaEffects::Acquire) == 0 ||
            IsHeldType(c.signatures_[entry.signature].returnType)) {
            continue;
        }

        binder_.Report(DiagnosticCode::InvalidBindingSchema, schema.source,
                       function.returnsSpan.end != 0 ? function.returnsSpan : function.span,
                       "An 'acquire' function returns a held resource type.",
                       "Acquiring returns the held resource that the caller keeps.",
                       "Return a type marked 'held: true', or remove 'acquire'.");
    }
}

} // namespace rocks::sparks::detail
