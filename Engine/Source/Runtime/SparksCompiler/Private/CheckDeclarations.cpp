#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "TypeChecker.h"
#include <Core/Graph/StrongComponents.h>

// 선언 규칙이다(Docs/Design/SparksLanguage.ko.md 3.11절): `?` 없는 객체 필드(S0428), 상수식(S0510)·
// const 순환(S0511)·const 타입(S0512), core 프로필의 full 문법(S1113). LC2d 단계(Flow)에서만 돈다.

namespace rocks::sparks::detail {

namespace {

// core 패키지에서 쓸 수 없는 attribute 이름이다(1.3절 full).
constexpr StringView FullOnlyAttributes[] = {
    "rpc", "worker", "deterministic", "pure", "tick", "cheat", "editorButton", "cvar", "test"};

} // namespace

bool TypeChecker::DeclaredBefore(uint32 left, uint32 right) const noexcept {
    const Compilation& c = compilation_;
    const Symbol& a = c.symbols_[left];
    const Symbol& b = c.symbols_[right];
    const int package =
        StringView::CompareBytes(c.packages_[c.modules_[a.module].package].manifest.Name(),
                                 c.packages_[c.modules_[b.module].package].manifest.Name());
    if (package != 0) {
        return package < 0;
    }

    const int path = StringView::CompareBytes(c.Text(c.modules_[a.module].importPath),
                                              c.Text(c.modules_[b.module].importPath));
    if (path != 0) {
        return path < 0;
    }

    return binder_.NodeSpan(a.module, a.nameNode).begin <
           binder_.NodeSpan(b.module, b.nameNode).begin;
}

bool TypeChecker::HasAttribute(uint32 module, uint32 declaration, StringView name) const noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    const uint32 attributes = tree.Child(declaration, 1);
    if (attributes == NoNode || tree.Node(attributes).kind != SyntaxKind::List) {
        return false;
    }

    for (const uint32 attribute : tree.Children(attributes)) {
        if (tree.Node(attribute).kind == SyntaxKind::Attribute &&
            TokenText(module, attribute) == name) {
            return true;
        }
    }

    return false;
}

uint32 TypeChecker::FirstNonConstant(uint32 module, uint32 root, bool allowComponent) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    // component 생성은 초기값 자체(괄호 안 포함)일 때만 받는다(원소·인자 안은 아니다).
    uint32 componentRoot = root;
    while (componentRoot != NoNode && tree.Node(componentRoot).kind == SyntaxKind::Parenthesized) {
        componentRoot = tree.Child(componentRoot, 0);
    }

    Array<uint32> pending(allocator_);
    auto push = [&](uint32 node) noexcept {
        if (node == NoNode) {
            return true;
        }

        if (auto pushed = pending.TryPushBack(node); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        return true;
    };
    // 자식을 거꾸로 넣어 왼쪽부터 꺼낸다.
    auto pushChildren = [&](uint32 node) noexcept {
        const uint32 count = tree.Node(node).childCount;
        for (uint32 index = count; index > 0; --index) {
            if (!push(tree.Child(node, index - 1))) {
                return false;
            }
        }

        return true;
    };
    if (!push(root)) {
        return NoNode;
    }

    while (!pending.IsEmpty()) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::IntegerLiteral:
        case SyntaxKind::FloatLiteral:
        case SyntaxKind::BoolLiteral:
        case SyntaxKind::NullLiteral:
        case SyntaxKind::VerbatimString:
        case SyntaxKind::Error:
            break;
        case SyntaxKind::String:
            for (const uint32 part : tree.Children(node)) {
                if (tree.Node(part).kind == SyntaxKind::Interpolation) {
                    return node; // 보간은 상수식이 아니다
                }
            }

            break;
        case SyntaxKind::Parenthesized:
        case SyntaxKind::Unary:
        case SyntaxKind::Binary:
        case SyntaxKind::CollectionLiteral:
        case SyntaxKind::MapEntry:
        case SyntaxKind::List:
            if (!pushChildren(node)) {
                return NoNode;
            }

            break;
        case SyntaxKind::Argument:
            if (!push(tree.Child(node, 1))) { // 이름 지정 인자의 이름은 식이 아니다
                return NoNode;
            }

            break;
        case SyntaxKind::Identifier: {
            const uint32 symbol = c.NodeSymbol(module, node);
            if (symbol != NoSymbol && c.symbols_[symbol].kind != SymbolKind::Const) {
                return node; // 지역·인자·필드·문맥 이름 등
            }

            break; // const 이름(해석하지 못한 이름은 앞에서 알렸다)
        }
        case SyntaxKind::Member: {
            const uint32 symbol = c.NodeSymbol(module, node);
            if (symbol == NoSymbol) {
                break; // 해석하지 못한 멤버(앞에서 알렸다)
            }

            const SymbolKind kind = c.symbols_[symbol].kind;
            if (kind != SymbolKind::EnumCase && kind != SymbolKind::Const) {
                return node;
            }

            break; // `Mode.A`·`Alias.Limit`(받는 쪽은 타입·별칭)
        }
        case SyntaxKind::Call: {
            const uint32 callee = tree.Child(node, 0);
            const uint32 symbol = callee == NoNode ? NoSymbol : c.NodeSymbol(module, callee);
            if (symbol == NoSymbol) {
                break; // 해석하지 못한 대상(앞에서 알렸다)
            }

            const Symbol& target = c.symbols_[symbol];
            bool allowed = false;
            switch (target.kind) {
            case SymbolKind::Struct:
            case SymbolKind::BuiltinType:  // 숫자 변환
            case SymbolKind::PreludeType:  // Rotator·Color·Transform·TagSet·Row 등
            case SymbolKind::PreludeValue: // 검증 리터럴(Tag·Action·Asset·Loc·CVar)
                allowed = target.kind != SymbolKind::PreludeValue ||
                          LanguageNames()[target.index].kind == LanguageNameKind::Literal;
                break;
            case SymbolKind::EngineType:
                allowed = IsEngineStruct(symbol) || (allowComponent && node == componentRoot);
                break;
            case SymbolKind::Class:
                allowed = allowComponent && node == componentRoot; // component 생성(필드 초기값)
                break;
            case SymbolKind::LanguageFunction:
                allowed = target.owner != NoSymbol && c.symbols_[target.owner].name == "Math" &&
                          (target.name == "Radians" || target.name == "Degrees");
                break;
            default:
                break;
            }

            if (!allowed) {
                return node;
            }

            if (!push(tree.Child(node, 1))) {
                return NoNode;
            }

            break;
        }
        default:
            return node; // this·원소·await·try·as? 등
        }
    }

    return NoNode;
}

bool TypeChecker::IsConstType(uint32 type) const noexcept {
    if (type == NoType || IsOptional(type)) {
        return false;
    }

    if (type == boolType_ || type == stringType_ || Numeric(type).kind != NumericKind::None ||
        IsEnumType(type) || IsVector(type)) {
        return true;
    }

    constexpr StringView Names[] = {"Tag", "quat", "Rotator", "Color", "Transform"};
    for (const StringView name : Names) {
        if (IsLanguage(type, name)) {
            return true;
        }
    }

    return false;
}

void TypeChecker::CheckDeclarationRules(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;
    const uint32 levelScript = binder_.FindName(SpaceKey(NameSpaceKind::Engine, 0), "LevelScript");
    const uint32 declarations = tree.Child(tree.Root(), 1);
    for (const uint32 node : tree.Children(declarations)) {
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        if (entry.kind == SyntaxKind::Const) {
            const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
            const uint32 type = symbol == NoSymbol ? NoType : c.symbols_[symbol].type;
            if (type != NoType && !IsConstType(type)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("A constant cannot have the type '");
                AppendDisplayType(message, type);
                message.Append("'.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::ConstType, tree.Child(node, 3), message.View(),
                           "Constants are booleans, numbers, strings, enums, tags and math types.",
                           "Use a 'let' field or a function instead.");
                }
            }

            const uint32 first = FirstNonConstant(module, tree.Child(node, 4), false);
            if (first != NoNode) {
                Report(context, DiagnosticCode::NotConstant, first,
                       "A constant's value must be a constant expression.",
                       "Constants are computed by the compiler from literals, constants, enum "
                       "cases and constructions of them.",
                       "Use literals and other constants, or a 'let' field set in 'on Start()'.");
            }

            continue;
        }

        if (entry.kind != SyntaxKind::Class && entry.kind != SyntaxKind::Struct) {
            continue;
        }

        const bool isClass = entry.kind == SyntaxKind::Class;
        const uint32 owner = c.NodeSymbol(module, tree.Child(node, 2));
        const uint32 members = tree.Child(node, isClass ? 4 : 3);
        if (owner == NoSymbol || members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
            continue;
        }

        const bool lifecycle = isClass && (binder_.EngineChainHas(owner, binder_.component_) ||
                                           binder_.EngineChainHas(owner, binder_.actor_));
        const bool inLevelScript = isClass && binder_.EngineChainHas(owner, levelScript);
        for (const uint32 member : tree.Children(members)) {
            if (tree.Node(member).kind != SyntaxKind::Field) {
                continue;
            }

            const uint32 field = c.NodeSymbol(module, tree.Child(member, 2));
            const uint32 initializer = tree.Child(member, 4);
            if (initializer != NoNode) {
                const uint32 first = FirstNonConstant(module, initializer, isClass);
                if (first != NoNode) {
                    Report(context, DiagnosticCode::NotConstant, first,
                           "A field's initial value must be a constant expression.",
                           "Initial values are stored in the class defaults; they are constants "
                           "or a component construction.",
                           "Set the value in 'on Start()' instead.");
                }
            }

            // LevelScript는 Collider component를 가질 수 없다(3.12절 S0802 — Rules 단계). 판정은
            // 생성식의 타입으로 한다(필드 표기와 무관).
            uint32 created = initializer;
            while (created != NoNode && tree.Node(created).kind == SyntaxKind::Parenthesized) {
                created = tree.Child(created, 0);
            }

            if (rules_ && inLevelScript && (tree.Node(member).flags & SyntaxFlags::Let) != 0 &&
                created != NoNode && tree.Node(created).kind == SyntaxKind::Call &&
                binder_.EngineChainHas(SymbolOf(c.ExpressionType(module, created)),
                                       EngineSymbol("Collider"))) {
                Report(context, DiagnosticCode::LevelScriptCollider, created,
                       "A LevelScript cannot own a collider.",
                       "A LevelScript has no physics. Place a trigger actor and handle its "
                       "events.",
                       "Place a trigger actor and use 'on Trigger.TriggerEntered(...)'.");
                continue;
            }

            const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
            if (type == NoType || !IsObjectType(type) || IsOptional(type)) {
                continue;
            }

            // (a) component 생성을 둔 `let` 필드, (b) Component·Actor 파생 class의 `@edit` 필드.
            const bool edit = HasAttribute(module, member, "edit");
            uint32 value = initializer;
            while (value != NoNode && tree.Node(value).kind == SyntaxKind::Parenthesized) {
                value = tree.Child(value, 0);
            }

            const bool ownedComponent =
                isClass && (tree.Node(member).flags & SyntaxFlags::Let) != 0 && value != NoNode &&
                tree.Node(value).kind == SyntaxKind::Call;
            if (edit && inLevelScript && !ownedComponent) {
                // LevelScript의 `@edit` 객체 필드는 `T?`만(3.12절 S0413 — Rules 단계).
                if (rules_) {
                    const uint32 editType = tree.Child(member, 3);
                    StringBuilder<CompilationMessageInlineBytes> editFix(allocator_);
                    editFix.Append("Declare it as '");
                    AppendDisplayType(editFix, type);
                    editFix.Append("?'.");
                    if (binder_.Succeeded(editFix)) {
                        Report(context, DiagnosticCode::LevelScriptRequiredObject,
                               editType != NoNode ? editType : tree.Child(member, 2),
                               "Object fields of a LevelScript must be optional.",
                               "Level actors can be destroyed during play, so a LevelScript reads "
                               "them through 'T?'.",
                               editFix.View());
                    }
                }

                continue;
            }

            if (ownedComponent || (edit && lifecycle)) {
                continue;
            }

            const uint32 typeNode = tree.Child(member, 3);
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Field '{}' must have a type with '?'.", c.symbols_[field].name);
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            fix.Append("Declare it as '");
            AppendDisplayType(fix, type);
            fix.Append("?'.");
            if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
                Report(context, DiagnosticCode::ObjectFieldNeedsOptional,
                       typeNode != NoNode ? typeNode : tree.Child(member, 2), message.View(),
                       "An object can be destroyed; only an owned component or an '@edit' "
                       "reference in the same prefab is never null.",
                       fix.View());
            }
        }
    }

    CheckProfile(module);
}

void TypeChecker::CheckProfile(uint32 module) noexcept {
    const Compilation& c = compilation_;
    if (c.packages_[c.modules_[module].package].manifest.GetProfile() != Profile::Core) {
        return;
    }

    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;
    auto report = [&](uint32 at, StringView feature, StringView alternative) noexcept {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("{} needs the full profile.", feature);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        fix.AppendFormat("Set profile: \"full\" in SparksPackage.json5, or {}.", alternative);
        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            Report(context, DiagnosticCode::FullProfileOnly, at, message.View(),
                   "This package uses profile: \"core\".", fix.View());
        }
    };
    for (uint32 node = 0; node < tree.NodeCount() && binder_.error_ == ErrorCode::None; ++node) {
        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::Interface:
            report(tree.Child(node, 2), "'interface'",
                   "share behavior through an engine base type and events");
            break;
        case SyntaxKind::Class: {
            if ((entry.flags & SyntaxFlags::Open) != 0) {
                report(tree.Child(node, 2), "'open class'",
                       "derive classes only from engine types");
            }

            // core의 기반은 엔진 타입뿐이다: Sparks class·interface 기반마다 그 기반 경로에 하나.
            const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
            const uint32 bases = tree.Child(node, 3);
            if (symbol == NoSymbol || bases == NoNode) {
                break;
            }

            const Symbol& classEntry = c.symbols_[symbol];
            for (uint32 index = 0; index < classEntry.baseCount; ++index) {
                const Symbol& base = c.symbols_[c.symbolBases_[classEntry.firstBase + index]];
                if (base.kind != SymbolKind::Class && base.kind != SymbolKind::Interface) {
                    continue;
                }

                for (const uint32 path : tree.Children(bases)) {
                    const uint32 count = tree.Node(path).childCount;
                    if (count != 0 && TokenText(module, tree.Child(path, count - 1)) == base.name) {
                        report(path,
                               base.kind == SymbolKind::Class ? StringView("A Sparks base class")
                                                              : StringView("A Sparks interface"),
                               "derive from an engine type");
                        break;
                    }
                }
            }

            break;
        }
        case SyntaxKind::Function:
            if ((entry.flags & SyntaxFlags::Open) != 0) {
                report(tree.Child(node, 2), "'open fn'", "keep the function in one class");
            }

            break;
        case SyntaxKind::Type: {
            const uint32 type = c.NodeType(module, node);
            if (type != NoType && IsLanguage(NonOptional(type), "Class")) {
                report(node, "'Class<T>'", "refer to a prefab instead");
            }

            break;
        }
        case SyntaxKind::Attribute:
            for (const StringView name : FullOnlyAttributes) {
                if (TokenText(module, node) == name) {
                    StringBuilder<CompilationMessageInlineBytes> feature(allocator_);
                    feature.AppendFormat("'@{}'", name);
                    if (binder_.Succeeded(feature)) {
                        report(node, feature.View(), "remove the attribute");
                    }
                }
            }

            break;
        case SyntaxKind::Handler: {
            const uint32 path = tree.Child(node, 2);
            if (path != NoNode && tree.Node(path).childCount == 1 &&
                tree.Node(tree.Child(path, 0)).kind == SyntaxKind::Name &&
                TokenText(module, tree.Child(path, 0)) == "Validate") {
                report(tree.Child(path, 0), "'on Validate()'", "check values in 'on Start()'");
            }

            break;
        }
        default:
            break;
        }
    }
}

void TypeChecker::CheckConstCycles() noexcept {
    Compilation& c = compilation_;
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    Array<uint32> consts(allocator_);
    Array<uint32> local(allocator_);
    if (auto resized = local.TryResize(symbolCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < symbolCount; ++id) {
        local[id] = NoLocalIndex;
        const Symbol& symbol = c.symbols_[id];
        if (symbol.kind == SymbolKind::Const && symbol.node != NoNode &&
            c.nodeBase_[symbol.module] != NoNode) {
            local[id] = static_cast<uint32>(consts.Size());
            if (auto pushed = consts.TryPushBack(id); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }

    // 간선: 초기값 안의 const 이름과 별칭 멤버(`Alias.X`)의 const.
    Array<uint32> firstEdge(allocator_);
    Array<uint32> targets(allocator_);
    Array<uint32> pending(allocator_);
    for (const uint32 id : consts) {
        if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(targets.Size())); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        const Symbol& symbol = c.symbols_[id];
        const SyntaxTree& tree = c.trees_[symbol.module];
        pending.Clear();
        if (auto pushed = pending.TryPushBack(tree.Child(symbol.node, 4)); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        while (!pending.IsEmpty()) {
            const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
            if (node == NoNode) {
                continue;
            }

            const SyntaxNode& entry = tree.Node(node);
            const uint32 target =
                entry.kind == SyntaxKind::Identifier || entry.kind == SyntaxKind::Member
                    ? c.NodeSymbol(symbol.module, node)
                    : NoSymbol;
            if (target != NoSymbol && local[target] != NoLocalIndex) {
                if (auto pushed = targets.TryPushBack(local[target]); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }

            for (uint32 child = 0; child < entry.childCount; ++child) {
                if (auto pushed = pending.TryPushBack(tree.Child(node, child)); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    if (auto pushed = firstEdge.TryPushBack(static_cast<uint32>(targets.Size())); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    auto visited = algo::ForEachStrongComponent(
        allocator_, static_cast<uint32>(consts.Size()), firstEdge.View(), targets.View(),
        [&](Span<const uint32> members, bool cyclic) noexcept -> Result<void> {
            if (!cyclic) {
                return Result<void>::Success();
            }

            uint32 first = consts[members[0]];
            for (const uint32 member : members) {
                first = DeclaredBefore(consts[member], first) ? consts[member] : first;
            }

            const Symbol& entry = c.symbols_[first];
            CheckContext context;
            context.module = entry.module;
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Constant '{}' depends on itself through its value.", entry.name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ConstCycle, entry.nameNode, message.View(),
                       "A constant is computed from other constants, so they cannot form a "
                       "cycle.",
                       "Write the value of one constant directly.");
            }

            return Result<void>::Success();
        });
    if (!visited) {
        binder_.error_ = visited.Error();
    }
}

} // namespace rocks::sparks::detail
