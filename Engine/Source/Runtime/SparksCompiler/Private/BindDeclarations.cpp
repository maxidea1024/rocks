#include <Core/Text/EditDistance.h>

#include <SparksCompiler/Token.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "NameBinder.h"
#include "Names.h"

// 선언 검사다: 모듈·멤버 선언, import 이름(S0311), 선언 타입과 시그니처, 기반 목록(S0322)·상속
// 순환(S0323), 멤버 이름(S0306–S0313), override(S0314–S0316), interface 구현(S0324), manifest
// services(S0325). 규칙: Docs/Design/SparksLanguage 3.9절.

namespace rocks::sparks::detail {

namespace {

// 어휘·구문 진단의 코드 범위다(3.9절 연쇄 방지의 "어휘·구문 진단").
constexpr uint32 FirstSyntaxCode = 101;
constexpr uint32 LastSyntaxCode = 299;

// 진단 문장에 넣는 선언 종류다.
StringView DeclarationWhat(SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
        return "Type name";
    case SymbolKind::Const:
        return "Constant name";
    case SymbolKind::Function:
        return "Function name";
    case SymbolKind::Field:
    case SymbolKind::Method:
    case SymbolKind::Event:
        return "Member name";
    case SymbolKind::EnumCase:
        return "Enum case";
    case SymbolKind::InterfaceMethod:
        return "Method name";
    default:
        return "Name"; // 별칭·매개변수·지역 이름은 부르는 쪽이 문장을 준다
    }
}

char FlipFirstLetter(char value) noexcept {
    if (detail::IsUpperAsciiLetter(value)) {
        return static_cast<char>(value - 'A' + 'a');
    }

    return detail::IsLowerAsciiLetter(value) ? static_cast<char>(value - 'a' + 'A') : value;
}

} // namespace

void NameBinder::CollectModuleStates() noexcept {
    Compilation& c = compilation_;
    const usize moduleCount = c.modules_.Size();
    if (auto resized = moduleStates_.TryResize(moduleCount); !resized) {
        error_ = resized.Error();
        return;
    }

    Array<uint32> moduleOfSource(allocator_);
    if (auto resized = moduleOfSource.TryResize(c.sources_->Count()); !resized) {
        error_ = resized.Error();
        return;
    }

    for (usize index = 0; index < moduleOfSource.Size(); ++index) {
        moduleOfSource[index] = NoModule;
    }

    for (uint32 module = 0; module < moduleCount; ++module) {
        moduleStates_[module] = ModuleState{};
        moduleOfSource[c.modules_[module].source.index] = module;
    }

    for (const Diagnostic& diagnostic : diagnostics_.View()) {
        const uint32 code = static_cast<uint32>(diagnostic.Code());
        if (code < FirstSyntaxCode || code > LastSyntaxCode ||
            diagnostic.Source().index >= moduleOfSource.Size()) {
            continue;
        }

        const uint32 module = moduleOfSource[diagnostic.Source().index];
        if (module == NoModule) {
            continue;
        }

        moduleStates_[module].syntaxErrors = true;
        if (diagnostic.Code() == DiagnosticCode::NestingTooDeep) {
            moduleStates_[module].tooDeep = true;
        }
    }

    for (uint32 module = 0; module < moduleCount; ++module) {
        ModuleState& state = moduleStates_[module];
        state.incomplete = state.syntaxErrors;
        for (const ImportResolution& import : c.ModuleImports(module)) {
            if (import.target == NoModule) {
                state.incomplete = true;
            } else if (moduleStates_[import.target].syntaxErrors) {
                state.incomplete = true;
            }
        }
    }
}

void NameBinder::ReportNameDiagnostic(DiagnosticCode code, uint32 symbol, StringView message,
                                      StringView explanation, StringView fix) noexcept {
    Symbol& entry = compilation_.symbols_[symbol];
    entry.flags |= SymbolFlags::Erroneous;
    Report(code, compilation_.modules_[entry.module].source, NodeSpan(entry.module, entry.nameNode),
           message, explanation, fix);
}

bool NameBinder::CheckDeclarationName(uint32 module, uint32 symbol, NameRule rule, StringView what,
                                      bool lifecycleMember, uint64 space) noexcept {
    static_cast<void>(module);
    const StringView name = compilation_.symbols_[symbol].name;
    if (name.IsEmpty() || error_ != ErrorCode::None) {
        return false;
    }

    // 1. 명명.
    const char first = name.Data()[0];
    const bool pascal = rule == NameRule::Pascal;
    if (pascal ? !detail::IsUpperAsciiLetter(first) : !detail::IsLowerAsciiLetter(first)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        StringBuilder<CompilationMessageInlineBytes> renamed(allocator_);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        message.AppendFormat("{} '{}' must start with {} letter.", what, name,
                             pascal ? StringView("an uppercase") : StringView("a lowercase"));
        const char flipped = FlipFirstLetter(first);
        renamed.Append(StringView(&flipped, 1))
            .Append(StringView(name.Data() + 1, name.ByteCount() - 1));
        if (IsKeywordText(renamed.View()) || IsReservedName(renamed.View())) {
            fix.Append(pascal ? StringView("Rename it so that it starts with an uppercase letter.")
                              : StringView("Rename it so that it starts with a lowercase letter."));
        } else {
            fix.AppendFormat("Rename it to '{}'.", renamed.View());
        }

        if (Succeeded(message) && Succeeded(renamed) && Succeeded(fix)) {
            ReportNameDiagnostic(DiagnosticCode::NamingRule, symbol, message.View(),
                                 pascal ? StringView("Types, members, enum cases and top-level "
                                                     "declarations use PascalCase.")
                                        : StringView("Parameters and local names use lowerCamel."),
                                 fix.View());
        }

        return false;
    }

    // 2. 예약.
    if (IsReservedName(name)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is a reserved name.", name);
        if (Succeeded(message)) {
            ReportNameDiagnostic(DiagnosticCode::ReservedName, symbol, message.View(),
                                 "Built-in types, prelude names, core engine types, context "
                                 "names and the like cannot be declared again.",
                                 "Choose another name.");
        }

        return false;
    }

    // 3. hook 이름.
    const StringView hook = lifecycleMember ? HookForMemberName(name) : StringView();
    if (!hook.IsEmpty()) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        message.AppendFormat("'{}' is a lifecycle hook name.", name);
        fix.AppendFormat("Handle the hook with 'on {}() {{ ... }}'.", hook);
        if (Succeeded(message) && Succeeded(fix)) {
            ReportNameDiagnostic(DiagnosticCode::HookName, symbol, message.View(),
                                 "Members of a lifecycle class cannot use hook names, so a "
                                 "function never looks like a hook that the engine calls.",
                                 fix.View());
        }

        return false;
    }

    // 4. 같은 선언 공간의 중복.
    if (space != NoSpace) {
        const uint32 previous = FindFolded(space, name);
        if (previous != NoSymbol && previous != symbol) {
            const Symbol& earlier = compilation_.symbols_[previous];
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            StringBuilder<CompilationMessageInlineBytes> explanation(allocator_);
            message.AppendFormat("'{}' is declared twice.", name);
            const SourceFile& file =
                compilation_.sources_->Get(compilation_.modules_[earlier.module].source);
            const SourceLocation at = file.Locate(NodeSpan(earlier.module, earlier.nameNode).begin);
            explanation.AppendFormat("The first declaration '{}' is at line {}; names in one "
                                     "place are compared ignoring letter case.",
                                     earlier.name, at.line);
            if (Succeeded(message) && Succeeded(explanation)) {
                ReportNameDiagnostic(DiagnosticCode::DuplicateDeclaration, symbol, message.View(),
                                     explanation.View(), "Rename one of them.");
            }

            return false;
        }
    }

    return true;
}

// 모듈 최상위 선언과 그 멤버를 심볼로 만든다. 최상위 이름은 여기서 검사하고(엔진 타입과의 충돌
// 포함), 멤버 이름은 상속을 안 뒤 CheckMembers가 검사한다.
void NameBinder::DeclareModule(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint32 declarations = tree.Child(tree.Root(), 1);
    const uint64 top = SpaceKey(NameSpaceKind::ModuleTop, module);
    const uint32 package = c.modules_[module].package;
    for (const uint32 node : tree.Children(declarations)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        Symbol symbol;
        switch (entry.kind) {
        case SyntaxKind::Class:
            symbol.kind = SymbolKind::Class;
            break;
        case SyntaxKind::Struct:
            symbol.kind = SymbolKind::Struct;
            break;
        case SyntaxKind::Enum:
            symbol.kind = SymbolKind::Enum;
            break;
        case SyntaxKind::Interface:
            symbol.kind = SymbolKind::Interface;
            break;
        case SyntaxKind::Const:
            symbol.kind = SymbolKind::Const;
            break;
        case SyntaxKind::Function:
            symbol.kind = SymbolKind::Function;
            break;
        default:
            continue; // 확장 이벤트는 이름을 도입하지 않는다(LC3), Error는 건너뛴다
        }

        const uint32 nameNode = tree.Child(node, 2);
        if (nameNode == NoNode || tree.Node(nameNode).kind != SyntaxKind::Name) {
            continue;
        }

        symbol.flags = ((entry.flags & SyntaxFlags::Open) != 0 ? SymbolFlags::Open : 0) |
                       ((entry.flags & SyntaxFlags::Override) != 0 ? SymbolFlags::Override : 0) |
                       ((entry.flags & SyntaxFlags::Async) != 0 ? SymbolFlags::Async : 0);
        symbol.name = NodeText(module, nameNode);
        symbol.module = module;
        symbol.node = node;
        symbol.nameNode = nameNode;
        const uint32 id = AddSymbol(symbol);
        if (id == NoSymbol) {
            return;
        }

        SetNodeSymbol(module, nameNode, id);
        InsertName(top, symbol.name, id);
        if (CheckDeclarationName(module, id, NameRule::Pascal, DeclarationWhat(symbol.kind), false,
                                 top)) {
            if (FindName(SpaceKey(NameSpaceKind::Engine, 0), symbol.name) != NoSymbol) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("'{}' is also the name of an engine type.", symbol.name);
                if (Succeeded(message)) {
                    ReportNameDiagnostic(DiagnosticCode::NameConflict, id, message.View(),
                                         "Engine types are visible in every module, so the name "
                                         "would mean two things.",
                                         "Choose another name.");
                }
            }
        }

        // import 제안용 패키지 이름 표.
        const NameKey key{package, symbol.name};
        if (uint32* existing = packageNames_.Find(key)) {
            *existing = AmbiguousSymbol;
        } else if (auto inserted = packageNames_.Insert(key, id); !inserted) {
            error_ = inserted.Error();
            return;
        }

        if (entry.kind == SyntaxKind::Class || entry.kind == SyntaxKind::Struct) {
            DeclareMembers(module, id, tree.Child(node, entry.kind == SyntaxKind::Class ? 4 : 3));
        } else if (entry.kind == SyntaxKind::Enum || entry.kind == SyntaxKind::Interface) {
            DeclareMembers(module, id, tree.Child(node, 3));
        }
    }
}

void NameBinder::DeclareMembers(uint32 module, uint32 owner, uint32 list) noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
        return;
    }

    const uint64 members = SpaceKey(NameSpaceKind::Members, owner);
    for (const uint32 node : tree.Children(list)) {
        const SyntaxNode& entry = tree.Node(node);
        Symbol symbol;
        switch (entry.kind) {
        case SyntaxKind::Field:
            symbol.kind = SymbolKind::Field;
            break;
        case SyntaxKind::Function:
            symbol.kind = SymbolKind::Method;
            break;
        case SyntaxKind::Event:
            symbol.kind = SymbolKind::Event;
            break;
        case SyntaxKind::EnumCase:
            symbol.kind = SymbolKind::EnumCase;
            break;
        case SyntaxKind::InterfaceMethod:
            symbol.kind = SymbolKind::InterfaceMethod;
            break;
        default:
            continue; // handler·Error
        }

        const uint32 nameNode = tree.Child(node, 2);
        if (nameNode == NoNode || tree.Node(nameNode).kind != SyntaxKind::Name) {
            continue;
        }

        symbol.flags = ((entry.flags & SyntaxFlags::Open) != 0 ? SymbolFlags::Open : 0) |
                       ((entry.flags & SyntaxFlags::Override) != 0 ? SymbolFlags::Override : 0) |
                       ((entry.flags & SyntaxFlags::Async) != 0 ? SymbolFlags::Async : 0) |
                       ((entry.flags & SyntaxFlags::Let) != 0 ? SymbolFlags::Let : 0);
        symbol.name = NodeText(module, nameNode);
        symbol.module = module;
        symbol.node = node;
        symbol.nameNode = nameNode;
        symbol.owner = owner;
        const uint32 id = AddSymbol(symbol);
        if (id == NoSymbol) {
            return;
        }

        SetNodeSymbol(module, nameNode, id);
        InsertName(members, symbol.name, id);
    }
}

// import가 도입하는 이름을 모듈의 import 공간에 더하고 충돌(S0311)을 본다.
void NameBinder::DeclareImports(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint64 imports = SpaceKey(NameSpaceKind::ModuleImport, module);
    const uint64 top = SpaceKey(NameSpaceKind::ModuleTop, module);
    const uint64 engine = SpaceKey(NameSpaceKind::Engine, 0);
    const SourceId source = c.modules_[module].source;
    for (const ImportResolution& import : c.ModuleImports(module)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        const uint32 aliasNode = tree.Child(import.node, 1);
        const bool aliased = aliasNode != NoNode && tree.Node(aliasNode).kind == SyntaxKind::Name;
        if (import.module == NoModule && import.target != NoModule && aliased &&
            c.nodeBase_[import.target] != NoNode) {
            // S0303·S0304를 받은 import의 별칭: 가리키는 모듈로 선언만 해 둔다(검사하지 않는다).
            Symbol symbol;
            symbol.kind = SymbolKind::Alias;
            symbol.flags = SymbolFlags::Erroneous;
            symbol.name = NodeText(module, aliasNode);
            symbol.module = module;
            symbol.node = import.node;
            symbol.nameNode = aliasNode;
            symbol.index = import.target;
            const uint32 id = AddSymbol(symbol);
            SetNodeSymbol(module, aliasNode, id);
            InsertName(imports, symbol.name, id);
            continue;
        }

        if (import.module == NoModule || c.nodeBase_[import.module] == NoNode) {
            continue;
        }

        if (aliased) {
            Symbol symbol;
            symbol.kind = SymbolKind::Alias;
            symbol.name = NodeText(module, aliasNode);
            symbol.module = module;
            symbol.node = import.node;
            symbol.nameNode = aliasNode;
            symbol.index = import.module;
            const uint32 id = AddSymbol(symbol);
            if (id == NoSymbol) {
                return;
            }

            SetNodeSymbol(module, aliasNode, id);
            if (CheckDeclarationName(module, id, NameRule::Pascal, "Import alias", false,
                                     NoSpace)) {
                const bool clash = FindName(imports, symbol.name) != NoSymbol ||
                                   FindName(top, symbol.name) != NoSymbol ||
                                   FindName(engine, symbol.name) != NoSymbol;
                if (clash) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.AppendFormat("Alias '{}' is already a name in this file.", symbol.name);
                    if (Succeeded(message)) {
                        ReportNameDiagnostic(DiagnosticCode::ImportConflict, id, message.View(),
                                             "An alias must differ from the declarations of the "
                                             "file, the names other imports bring and the engine "
                                             "types.",
                                             "Choose another alias.");
                    }
                }
            }

            InsertName(imports, symbol.name, id);
            continue;
        }

        // 대상 모듈의 최상위 선언을 원문 순서로 도입한다.
        const SyntaxTree& targetTree = c.trees_[import.module];
        const uint32 declarations = targetTree.Child(targetTree.Root(), 1);
        bool reported = false;
        for (const uint32 node : targetTree.Children(declarations)) {
            const SyntaxNode& entry = targetTree.Node(node);
            if (entry.kind == SyntaxKind::Error || entry.kind == SyntaxKind::ExtensionEvent ||
                entry.childCount < 3) {
                continue;
            }

            const uint32 nameNode = targetTree.Child(node, 2);
            const uint32 id = nameNode == NoNode ? NoSymbol : c.NodeSymbol(import.module, nameNode);
            if (id == NoSymbol) {
                continue;
            }

            const Symbol& symbol = c.symbols_[id];
            const bool fromImport = FindName(imports, symbol.name) != NoSymbol;
            const bool fromFile = FindName(top, symbol.name) != NoSymbol;
            if (!reported && (symbol.flags & SymbolFlags::Erroneous) == 0 &&
                (fromImport || fromFile)) {
                reported = true;
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat(fromFile ? StringView("This import brings '{}', which is "
                                                           "also declared in this file.")
                                              : StringView("This import brings '{}', which "
                                                           "another import already brings."),
                                     symbol.name);
                if (Succeeded(message)) {
                    const uint32 path = tree.Child(import.node, 0);
                    Report(DiagnosticCode::ImportConflict, source,
                           SourceSpan{tree.Node(path).span.begin, tree.Node(path).span.begin},
                           message.View(),
                           "Each name in a file has one meaning; an import brings every "
                           "top-level declaration of its module.",
                           "Import the module with an alias, as in 'import Weapons.Sword as "
                           "Blades;', or rename the declaration.");
                }
            }

            InsertName(imports, symbol.name, id);
        }
    }
}

uint32 NameBinder::ResolveSignature(const BodyContext& context, uint32 symbol, uint32 parameters,
                                    uint32 returnType, bool isAsync) noexcept {
    const uint32 module = context.module;
    const SyntaxTree& tree = compilation_.trees_[module];
    Array<uint32> types(allocator_);
    if (parameters != NoNode && tree.Node(parameters).kind == SyntaxKind::List) {
        for (const uint32 parameter : tree.Children(parameters)) {
            if (tree.Node(parameter).kind != SyntaxKind::Parameter) {
                continue;
            }

            const uint32 type = ResolveTypeNode(context, tree.Child(parameter, 1));
            if (auto pushed = types.TryPushBack(type); !pushed) {
                error_ = pushed.Error();
                return NoSignature;
            }

            const uint32 nameNode = tree.Child(parameter, 0);
            if (nameNode == NoNode || tree.Node(nameNode).kind != SyntaxKind::Name) {
                continue;
            }

            Symbol entry;
            entry.kind = SymbolKind::Parameter;
            entry.name = NodeText(module, nameNode);
            entry.module = module;
            entry.node = parameter;
            entry.nameNode = nameNode;
            entry.owner = symbol;
            entry.type = type;
            const uint32 id = AddSymbol(entry);
            SetNodeSymbol(module, nameNode, id);
        }
    }

    const uint32 result = returnType == NoNode ? voidType_ : ResolveTypeNode(context, returnType);
    return AddSignature(result, types.View(), isAsync);
}

// 본문이 없는 매개변수 목록(이벤트·interface 메서드·확장 이벤트)의 이름을 검사한다(명명·예약·
// 중복). 함수·handler 매개변수는 본문의 지역 이름으로 BindBodies가 검사한다.
void NameBinder::DeclareParameters(const BodyContext& context, uint32 owner, uint32 list) noexcept {
    static_cast<void>(owner);
    const SyntaxTree& tree = compilation_.trees_[context.module];
    if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
        return;
    }

    const Span<const uint32> parameters = tree.Children(list);
    for (usize index = 0; index < parameters.Size() && error_ == ErrorCode::None; ++index) {
        if (tree.Node(parameters[index]).kind != SyntaxKind::Parameter) {
            continue;
        }

        const uint32 nameNode = tree.Child(parameters[index], 0);
        const uint32 id =
            nameNode == NoNode ? NoSymbol : compilation_.NodeSymbol(context.module, nameNode);
        if (id == NoSymbol || !CheckDeclarationName(context.module, id, NameRule::Camel,
                                                    "Parameter name", false, NoSpace)) {
            continue;
        }

        const StringView name = compilation_.symbols_[id].name;
        for (usize earlier = 0; earlier < index; ++earlier) {
            const uint32 earlierName = tree.Node(parameters[earlier]).kind == SyntaxKind::Parameter
                                           ? tree.Child(parameters[earlier], 0)
                                           : NoNode;
            if (earlierName == NoNode || tree.Node(earlierName).kind != SyntaxKind::Name ||
                !EqualsIgnoringAsciiCase(NodeText(context.module, earlierName), name)) {
                continue;
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is declared twice.", name);
            if (Succeeded(message)) {
                ReportNameDiagnostic(DiagnosticCode::DuplicateDeclaration, id, message.View(),
                                     "Parameter names of one list are compared ignoring letter "
                                     "case.",
                                     "Rename one of them.");
            }

            break;
        }
    }
}

void NameBinder::ResolveDeclarationTypes(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint32 declarations = tree.Child(tree.Root(), 1);
    BodyContext moduleContext;
    moduleContext.module = module;
    moduleContext.reportUnknown = !moduleStates_[module].incomplete;
    for (const uint32 node : tree.Children(declarations)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        const uint32 nameNode = entry.childCount > 2 ? tree.Child(node, 2) : NoNode;
        const uint32 symbol = nameNode == NoNode ? NoSymbol : c.NodeSymbol(module, nameNode);
        switch (entry.kind) {
        case SyntaxKind::Const:
            if (symbol != NoSymbol) {
                c.symbols_[symbol].type = ResolveTypeNode(moduleContext, tree.Child(node, 3));
            } else {
                static_cast<void>(ResolveTypeNode(moduleContext, tree.Child(node, 3)));
            }

            break;
        case SyntaxKind::Function: {
            const uint32 signature =
                ResolveSignature(moduleContext, symbol, tree.Child(node, 3), tree.Child(node, 4),
                                 (entry.flags & SyntaxFlags::Async) != 0);
            if (symbol != NoSymbol) {
                c.symbols_[symbol].signature = signature;
            }

            break;
        }
        case SyntaxKind::ExtensionEvent: {
            // 확장 이벤트 심볼(owner = 대상 엔진 타입 Actor·World, 그 밖이면 NoSymbol — S0813)이다.
            // 이름 공간에 넣지 않으며 `on`·`emit`이 import로 찾는다(3.12절).
            const uint32 targetNode = tree.Child(node, 2);
            const uint32 nameNode = tree.Child(node, 3);
            uint32 event = NoSymbol;
            if (nameNode != NoNode && tree.Node(nameNode).kind == SyntaxKind::Name) {
                const uint32 target = targetNode == NoNode
                                          ? NoSymbol
                                          : LookupType(module, NodeText(module, targetNode));
                const uint32 world = FindName(SpaceKey(NameSpaceKind::Engine, 0), "World");
                Symbol entry;
                entry.kind = SymbolKind::Event;
                entry.name = NodeText(module, nameNode);
                entry.module = module;
                entry.node = node;
                entry.nameNode = nameNode;
                entry.owner =
                    target != NoSymbol && (target == actor_ || target == world) ? target : NoSymbol;
                event = AddSymbol(entry);
            }

            const uint32 signature =
                ResolveSignature(moduleContext, event, tree.Child(node, 4), NoNode, false);
            if (event != NoSymbol) {
                c.symbols_[event].signature = signature;
            }

            DeclareParameters(moduleContext, event, tree.Child(node, 4));
            break;
        }
        case SyntaxKind::Class:
        case SyntaxKind::Struct:
        case SyntaxKind::Interface: {
            BodyContext typeContext = moduleContext;
            typeContext.owner = symbol;
            // 기반을 모르는 class 안에서는 찾지 못함을 내지 않는다(3.9절 연쇄 방지).
            if (symbol != NoSymbol && (c.symbols_[symbol].flags & SymbolFlags::Incomplete) != 0) {
                typeContext.reportUnknown = false;
            }

            const uint32 members = tree.Child(node, entry.kind == SyntaxKind::Class ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                break;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& memberEntry = tree.Node(member);
                const uint32 memberName =
                    memberEntry.childCount > 2 ? tree.Child(member, 2) : NoNode;
                const uint32 memberSymbol =
                    memberName == NoNode ? NoSymbol : c.NodeSymbol(module, memberName);
                switch (memberEntry.kind) {
                case SyntaxKind::Field: {
                    const uint32 typeNode = tree.Child(member, 3);
                    const uint32 type =
                        typeNode == NoNode ? NoType : ResolveTypeNode(typeContext, typeNode);
                    if (memberSymbol != NoSymbol) {
                        c.symbols_[memberSymbol].type = type;
                    }

                    break;
                }
                case SyntaxKind::Function:
                case SyntaxKind::InterfaceMethod: {
                    const bool isMethod = memberEntry.kind == SyntaxKind::InterfaceMethod;
                    const uint32 signature = ResolveSignature(
                        typeContext, memberSymbol, tree.Child(member, 3), tree.Child(member, 4),
                        (memberEntry.flags & SyntaxFlags::Async) != 0);
                    if (memberSymbol != NoSymbol) {
                        c.symbols_[memberSymbol].signature = signature;
                    }

                    if (isMethod) {
                        DeclareParameters(typeContext, memberSymbol, tree.Child(member, 3));
                    }

                    break;
                }
                case SyntaxKind::Event: {
                    const uint32 signature = ResolveSignature(typeContext, memberSymbol,
                                                              tree.Child(member, 3), NoNode, false);
                    if (memberSymbol != NoSymbol) {
                        c.symbols_[memberSymbol].signature = signature;
                    }

                    DeclareParameters(typeContext, memberSymbol, tree.Child(member, 3));
                    break;
                }
                case SyntaxKind::Handler: {
                    // handler 매개변수 타입은 본문 해석이 지역 이름과 함께 해석한다.
                    break;
                }
                default:
                    break;
                }
            }

            break;
        }
        default:
            break;
        }
    }
}

void NameBinder::ResolveModuleBases(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint32 declarations = tree.Child(tree.Root(), 1);
    for (const uint32 node : tree.Children(declarations)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        if (tree.Node(node).kind != SyntaxKind::Class) {
            continue;
        }

        const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
        if (symbol != NoSymbol) {
            ResolveBases(module, symbol, tree.Child(node, 3));
        }
    }
}

void NameBinder::ResolveBases(uint32 module, uint32 classSymbol, uint32 list) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    BodyContext context;
    context.module = module;
    context.reportUnknown = !moduleStates_[module].incomplete;
    const uint32 firstBase = static_cast<uint32>(c.symbolBases_.Size());
    bool firstResolved = false;
    if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
        c.symbols_[classSymbol].flags |= SymbolFlags::Incomplete;
        return;
    }

    const Span<const uint32> paths = tree.Children(list);
    for (usize index = 0; index < paths.Size() && error_ == ErrorCode::None; ++index) {
        const uint32 path = paths[index];
        if (tree.Node(path).kind != SyntaxKind::Path) {
            continue;
        }

        const uint32 base = ResolveTypePath(context, path);
        SetNodeType(module, path, InternType(base, Span<const uint32>(), false));
        if (base == NoSymbol) {
            continue;
        }

        const Symbol& entry = c.symbols_[base];
        StringView problem;
        if (index == 0) {
            bool extensible = false;
            if (entry.kind == SymbolKind::EngineType) {
                const SchemaType& type = c.schemas_[entry.index].schema.Types()[entry.item];
                extensible = type.scriptExtensible && (type.kind == SchemaTypeKind::Object ||
                                                       type.kind == SchemaTypeKind::Asset);
            } else if (entry.kind == SymbolKind::Class) {
                extensible = (entry.flags & SymbolFlags::Open) != 0;
            }

            if (!extensible) {
                problem = entry.kind == SymbolKind::Class
                              ? StringView("Class '{}' is not open, so it cannot be a base class.")
                              : StringView("'{}' cannot be a base class.");
            } else {
                firstResolved = true;
                c.symbols_[classSymbol].base = base;
            }
        } else {
            const bool isInterface = entry.kind == SymbolKind::Interface ||
                                     (entry.kind == SymbolKind::EngineType &&
                                      c.schemas_[entry.index].schema.Types()[entry.item].kind ==
                                          SchemaTypeKind::Interface);
            bool repeated = false;
            for (uint32 at = firstBase; at < c.symbolBases_.Size(); ++at) {
                repeated = repeated || c.symbolBases_[at] == base;
            }

            if (!isInterface) {
                problem = StringView("'{}' is not an interface; only the first base is a class.");
            } else if (repeated) {
                problem = StringView("Interface '{}' is listed twice.");
            }
        }

        if (!problem.IsEmpty()) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat(problem, entry.name);
            if (Succeeded(message)) {
                Report(DiagnosticCode::InvalidBase, c.modules_[module].source, tree.Node(path).span,
                       message.View(),
                       "The first base is an engine type that scripts may extend (or an open "
                       "class), and the others are interfaces.",
                       "Fix the base list, as in 'class Door : ScriptableComponent'.");
            }

            continue;
        }

        if (auto pushed = c.symbolBases_.TryPushBack(base); !pushed) {
            error_ = pushed.Error();
            return;
        }
    }

    Symbol& symbol = c.symbols_[classSymbol];
    symbol.firstBase = firstBase;
    symbol.baseCount = static_cast<uint32>(c.symbolBases_.Size()) - firstBase;
    if (!firstResolved) {
        symbol.flags |= SymbolFlags::Incomplete;
    }
}

// Sparks 클래스 상속 순환(S0323), 불완전한 class, 수명 클래스 표시다.
void NameBinder::CheckInheritance() noexcept {
    Compilation& c = compilation_;
    const uint32 count = static_cast<uint32>(c.symbols_.Size());
    Array<uint8> state(allocator_); // 0 미방문, 1 걷는 중, 2 끝
    Array<uint32> path(allocator_);
    if (auto resized = state.TryResize(count); !resized) {
        error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < count; ++id) {
        state[id] = 0;
    }

    auto isClass = [&c](uint32 id) noexcept {
        return id != NoSymbol && c.symbols_[id].kind == SymbolKind::Class;
    };
    // (패키지 이름, 모듈 import 경로, 원문 위치) byte 순서로 앞인지다.
    auto before = [&c, this](uint32 left, uint32 right) noexcept {
        const Symbol& a = c.symbols_[left];
        const Symbol& b = c.symbols_[right];
        const int package =
            StringView::CompareBytes(c.packages_[c.modules_[a.module].package].manifest.Name(),
                                     c.packages_[c.modules_[b.module].package].manifest.Name());
        if (package != 0) {
            return package < 0;
        }

        const int order = StringView::CompareBytes(c.Text(c.modules_[a.module].importPath),
                                                   c.Text(c.modules_[b.module].importPath));
        if (order != 0) {
            return order < 0;
        }

        return NodeSpan(a.module, a.nameNode).begin < NodeSpan(b.module, b.nameNode).begin;
    };
    for (uint32 start = 0; start < count && error_ == ErrorCode::None; ++start) {
        if (!isClass(start) || state[start] != 0) {
            continue;
        }

        path.Clear();
        uint32 at = start;
        while (isClass(at) && state[at] == 0) {
            state[at] = 1;
            if (auto pushed = path.TryPushBack(at); !pushed) {
                error_ = pushed.Error();
                return;
            }

            at = c.symbols_[at].base;
        }

        if (isClass(at) && state[at] == 1) {
            uint32 first = at;
            for (uint32 member = c.symbols_[at].base; member != at;
                 member = c.symbols_[member].base) {
                first = before(member, first) ? member : first;
            }

            const Symbol& reported = c.symbols_[first];
            const SyntaxTree& tree = c.trees_[reported.module];
            const uint32 basePath = tree.Child(tree.Child(reported.node, 3), 0);
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Class '{}' inherits from itself through its base classes.",
                                 reported.name);
            if (Succeeded(message)) {
                Report(DiagnosticCode::InheritanceCycle, c.modules_[reported.module].source,
                       tree.Node(basePath).span, message.View(),
                       "A class cannot be its own base, directly or through other classes.",
                       "Remove one of the base classes in the cycle.");
            }

            // 순환을 끊고 그 class들을 불완전으로 둔다.
            uint32 member = at;
            do {
                const uint32 next = c.symbols_[member].base;
                c.symbols_[member].flags |= SymbolFlags::Incomplete;
                c.symbols_[member].base = NoSymbol;
                member = next;
            } while (member != at && member != NoSymbol);
        }

        for (const uint32 walked : path) {
            state[walked] = 2;
        }
    }

    // 불완전(Sparks 기반이 불완전)·수명 표시. 기반 사슬은 이제 순환이 없다.
    for (uint32 id = 0; id < count; ++id) {
        if (!isClass(id)) {
            continue;
        }

        uint32 at = id;
        bool incomplete = false;
        while (isClass(at)) {
            incomplete = incomplete || (c.symbols_[at].flags & SymbolFlags::Incomplete) != 0;
            at = c.symbols_[at].base;
        }

        if (incomplete) {
            c.symbols_[id].flags |= SymbolFlags::Incomplete;
        } else if (EngineChainHas(at, actor_) || EngineChainHas(at, component_) ||
                   EngineChainHas(at, widgetController_) || EngineChainHas(at, worldService_)) {
            c.symbols_[id].flags |= SymbolFlags::Lifecycle;
        }
    }
}

uint32 NameBinder::ContextNamesOf(uint32 classSymbol) const noexcept {
    const Compilation& c = compilation_;
    if (classSymbol == NoSymbol || c.symbols_[classSymbol].kind != SymbolKind::Class) {
        return 0;
    }

    uint32 at = classSymbol;
    while (at != NoSymbol && c.symbols_[at].kind == SymbolKind::Class) {
        at = c.symbols_[at].base;
    }

    if (EngineChainHas(at, actor_) || EngineChainHas(at, component_) ||
        EngineChainHas(at, widgetController_)) {
        return ContextBits::World | ContextBits::Session | ContextBits::Input | ContextBits::Time;
    }

    if (EngineChainHas(at, worldService_)) {
        return ContextBits::World | ContextBits::Session | ContextBits::Time;
    }

    return 0;
}

uint32 NameBinder::FindInherited(uint32 owner, StringView name, bool foldSparks) const noexcept {
    const Compilation& c = compilation_;
    uint32 at = c.symbols_[owner].base;
    for (uint32 step = 0; at != NoSymbol && step < c.symbols_.Size(); ++step) {
        const uint64 space = SpaceKey(NameSpaceKind::Members, at);
        const bool sparks = c.symbols_[at].kind == SymbolKind::Class;
        const uint32 found = sparks && foldSparks ? FindFolded(space, name) : FindName(space, name);
        if (found != NoSymbol) {
            return found;
        }

        at = c.symbols_[at].base;
    }

    return NoSymbol;
}

uint32 NameBinder::FindMemberInChain(uint32 type, StringView name) const noexcept {
    const Compilation& c = compilation_;
    for (uint32 step = 0; type != NoSymbol && step < c.symbols_.Size(); ++step) {
        const uint32 found = FindName(SpaceKey(NameSpaceKind::Members, type), name);
        if (found != NoSymbol) {
            return found;
        }

        type = c.symbols_[type].base;
    }

    return NoSymbol;
}

bool NameBinder::IsFunctionSymbol(uint32 symbol) const noexcept {
    if (symbol == NoSymbol) {
        return false;
    }

    const SymbolKind kind = compilation_.symbols_[symbol].kind;
    return kind == SymbolKind::Method || kind == SymbolKind::EngineFunction ||
           kind == SymbolKind::InterfaceMethod || kind == SymbolKind::Function;
}

// 시그니처가 같은지다. 해석하지 못한 타입이 끼면 같다고 본다(연쇄 진단 없음).
bool NameBinder::SameSignature(uint32 left, uint32 right) const noexcept {
    const Compilation& c = compilation_;
    if (left == NoSignature || right == NoSignature) {
        return true;
    }

    const Signature& a = c.signatures_[left];
    const Signature& b = c.signatures_[right];
    // 해석하지 못한 매개변수·반환 타입이 있으면 비교하지 않는다(그 타입의 진단이 이미 있다).
    auto resolved = [&c](const Signature& signature) noexcept {
        if (signature.returnType == NoType) {
            return false;
        }

        for (uint32 index = 0; index < signature.parameterCount; ++index) {
            if (c.signatureParameters_[signature.firstParameter + index] == NoType) {
                return false;
            }
        }

        return true;
    };
    if (!resolved(a) || !resolved(b)) {
        return true;
    }

    if (a.parameterCount != b.parameterCount || a.isAsync != b.isAsync ||
        a.returnType != b.returnType) {
        return false;
    }

    for (uint32 index = 0; index < a.parameterCount; ++index) {
        if (c.signatureParameters_[a.firstParameter + index] !=
            c.signatureParameters_[b.firstParameter + index]) {
            return false;
        }
    }

    return true;
}

void NameBinder::CheckMembers(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint32 declarations = tree.Child(tree.Root(), 1);
    for (const uint32 node : tree.Children(declarations)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        if (entry.kind != SyntaxKind::Class && entry.kind != SyntaxKind::Struct &&
            entry.kind != SyntaxKind::Enum && entry.kind != SyntaxKind::Interface) {
            if (entry.kind == SyntaxKind::Function) {
                const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
                if (symbol != NoSymbol) {
                    CheckMember(module, NoSymbol, symbol);
                }
            }

            continue;
        }

        const uint32 owner = c.NodeSymbol(module, tree.Child(node, 2));
        if (owner == NoSymbol) {
            continue;
        }

        const uint32 members = tree.Child(node, entry.kind == SyntaxKind::Class ? 4 : 3);
        if (members != NoNode && tree.Node(members).kind == SyntaxKind::List) {
            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& memberEntry = tree.Node(member);
                if (memberEntry.childCount < 3 || memberEntry.kind == SyntaxKind::Handler) {
                    continue;
                }

                const uint32 symbol = c.NodeSymbol(module, tree.Child(member, 2));
                if (symbol != NoSymbol) {
                    CheckMember(module, owner, symbol);
                }
            }
        }

        if (entry.kind == SyntaxKind::Class) {
            CheckInterfaces(owner);
        }
    }
}

// 멤버 하나(최상위 fn은 owner가 NoSymbol — `open`·`override` 검사만)를 검사한다.
void NameBinder::CheckMember(uint32 module, uint32 owner, uint32 member) noexcept {
    Compilation& c = compilation_;
    const Symbol entry = c.symbols_[member];
    if ((entry.flags & SymbolFlags::Erroneous) != 0) {
        return; // 이름에 이미 진단을 냈다(선언 이름 하나에 하나)
    }

    if (owner == NoSymbol) {
        // 최상위 fn: 이름 검사는 DeclareModule이 했다.
        if ((entry.flags & SymbolFlags::Override) != 0) {
            ReportNameDiagnostic(DiagnosticCode::OverrideWithoutTarget, member,
                                 "A top-level function has nothing to override.",
                                 "Only functions of a class override base class functions.",
                                 "Remove 'override'.");
        } else if ((entry.flags & SymbolFlags::Open) != 0) {
            ReportNameDiagnostic(DiagnosticCode::MisplacedOpen, member,
                                 "A top-level function cannot be 'open'.",
                                 "'open' lets classes derived from an open class override a "
                                 "function.",
                                 "Remove 'open'.");
        }

        return;
    }

    const Symbol& ownerEntry = c.symbols_[owner];
    const bool isClass = ownerEntry.kind == SymbolKind::Class;
    const bool ownerIncomplete = (ownerEntry.flags & SymbolFlags::Incomplete) != 0;
    const bool lifecycle = isClass && (ownerEntry.flags & SymbolFlags::Lifecycle) != 0;
    if (!CheckDeclarationName(module, member, NameRule::Pascal, DeclarationWhat(entry.kind),
                              lifecycle && entry.kind != SymbolKind::EnumCase,
                              SpaceKey(NameSpaceKind::Members, owner))) {
        return;
    }

    if (entry.kind == SymbolKind::EnumCase || entry.kind == SymbolKind::InterfaceMethod) {
        return;
    }

    if (entry.kind == SymbolKind::Method && (entry.flags & SymbolFlags::Open) != 0 &&
        !(isClass && (ownerEntry.flags & SymbolFlags::Open) != 0)) {
        ReportNameDiagnostic(DiagnosticCode::MisplacedOpen, member,
                             isClass ? StringView("An 'open' function needs an 'open' class.")
                                     : StringView("A struct function cannot be 'open'."),
                             "'open' lets classes derived from an open class override a "
                             "function.",
                             isClass ? StringView("Add 'open' to the class or remove it here.")
                                     : StringView("Remove 'open'."));
        return;
    }

    if (entry.kind == SymbolKind::Method && (entry.flags & SymbolFlags::Override) != 0) {
        CheckOverride(module, owner, member);
        return;
    }

    // 5. 충돌: 상속 멤버(S0313) → 타입 이름(S0312) → 그 밖(S0310).
    if (isClass && !ownerIncomplete) {
        const uint32 inherited = FindInherited(owner, entry.name, true);
        if (inherited != NoSymbol) {
            const Symbol& base = c.symbols_[inherited];
            const Symbol& baseOwner = c.symbols_[base.owner];
            const bool overridable =
                entry.kind == SymbolKind::Method &&
                ((base.kind == SymbolKind::EngineFunction &&
                  (base.flags & SymbolFlags::Overridable) != 0) ||
                 (base.kind == SymbolKind::Method &&
                  (base.flags & (SymbolFlags::Open | SymbolFlags::Override)) != 0)) &&
                base.name == entry.name && SameSignature(entry.signature, base.signature);
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            message.AppendFormat("'{}' hides the member '{}' of the base type '{}'.", entry.name,
                                 base.name, baseOwner.name);
            if (overridable) {
                fix.AppendFormat("Write 'override fn {}(...)' to override it.", entry.name);
            } else {
                fix.Append("Rename the member.");
            }

            if (Succeeded(message) && Succeeded(fix)) {
                ReportNameDiagnostic(DiagnosticCode::HidesInheritedMember, member, message.View(),
                                     "A member cannot have the name of an inherited member.",
                                     fix.View());
            }

            return;
        }
    }

    const uint32 type = LookupType(module, entry.name);
    if (type != NoSymbol) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        message.AppendFormat("Member '{}' has the same name as a type.", entry.name);
        fix.AppendFormat("Rename the member, as in '{}Ref'.", entry.name);
        if (Succeeded(message) && Succeeded(fix)) {
            ReportNameDiagnostic(DiagnosticCode::MemberTypeName, member, message.View(),
                                 "Inside the type the name would mean both the member and the "
                                 "type.",
                                 fix.View());
        }

        return;
    }

    const uint32 other = [&]() noexcept {
        const uint32 top = FindName(SpaceKey(NameSpaceKind::ModuleTop, module), entry.name);
        return top != NoSymbol
                   ? top
                   : FindName(SpaceKey(NameSpaceKind::ModuleImport, module), entry.name);
    }();
    if (other != NoSymbol) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Member '{}' has the same name as a top-level declaration or import "
                             "of this file.",
                             entry.name);
        if (Succeeded(message)) {
            ReportNameDiagnostic(DiagnosticCode::NameConflict, member, message.View(),
                                 "Inside the type the name would mean two things.",
                                 "Rename the member.");
        }

        return;
    }

    if (isClass && !ownerIncomplete) {
        const uint32 available = ContextNamesOf(owner);
        for (uint32 index = 0; index < sizeof(ContextNames) / sizeof(ContextNames[0]); ++index) {
            if ((available & (1u << index)) == 0 ||
                !EqualsIgnoringAsciiCase(ContextNames[index], entry.name)) {
                continue;
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Member '{}' differs from the context name '{}' only in letter "
                                 "case.",
                                 entry.name, ContextNames[index]);
            if (Succeeded(message)) {
                ReportNameDiagnostic(DiagnosticCode::NameConflict, member, message.View(),
                                     "Value names that differ only in letter case are too easy "
                                     "to confuse.",
                                     "Rename the member.");
            }

            return;
        }
    }
}

void NameBinder::CheckOverride(uint32 module, uint32 owner, uint32 member) noexcept {
    static_cast<void>(module);
    Compilation& c = compilation_;
    const Symbol entry = c.symbols_[member];
    const Symbol& ownerEntry = c.symbols_[owner];
    if (ownerEntry.kind != SymbolKind::Class) {
        ReportNameDiagnostic(DiagnosticCode::OverrideWithoutTarget, member,
                             "A struct function has nothing to override.",
                             "Only functions of a class override base class functions.",
                             "Remove 'override'.");
        return;
    }

    if ((ownerEntry.flags & SymbolFlags::Incomplete) != 0) {
        return; // 기반을 모른다
    }

    const uint32 target = FindInherited(owner, entry.name, false);
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    if (target == NoSymbol) {
        message.AppendFormat("'{}' overrides nothing: no base type has a function '{}'.",
                             entry.name, entry.name);
        if (Succeeded(message)) {
            ReportNameDiagnostic(DiagnosticCode::OverrideWithoutTarget, member, message.View(),
                                 "'override fn' replaces a function of a base type with the same "
                                 "name.",
                                 "Remove 'override' or fix the name.");
        }

        return;
    }

    const Symbol& base = c.symbols_[target];
    const Symbol& baseOwner = c.symbols_[base.owner];
    if (!IsFunctionSymbol(target)) {
        message.AppendFormat("'{}.{}' is not a function.", baseOwner.name, base.name);
    } else if (!((base.kind == SymbolKind::EngineFunction &&
                  (base.flags & SymbolFlags::Overridable) != 0) ||
                 (base.kind == SymbolKind::Method &&
                  (base.flags & (SymbolFlags::Open | SymbolFlags::Override)) != 0))) {
        message.AppendFormat("'{}.{}' cannot be overridden.", baseOwner.name, base.name);
    }

    if (!message.View().IsEmpty()) {
        if (Succeeded(message)) {
            ReportNameDiagnostic(DiagnosticCode::OverrideWithoutTarget, member, message.View(),
                                 "Only overridable engine functions and 'open' functions of open "
                                 "classes can be overridden.",
                                 "Remove 'override' and rename the function.");
        }

        return;
    }

    if (!SameSignature(entry.signature, base.signature)) {
        message.AppendFormat("'{}' does not match the signature of '{}.{}'.", entry.name,
                             baseOwner.name, base.name);
        if (Succeeded(message)) {
            ReportNameDiagnostic(DiagnosticCode::OverrideSignatureMismatch, member, message.View(),
                                 "An override has the same parameter types, return type and "
                                 "'async' as the function it replaces; parameter names may "
                                 "differ.",
                                 "Use the signature of the base function.");
        }
    }
}

// class가 기반 interface의 메서드를 모두 구현했는지다(S0324).
void NameBinder::CheckInterfaces(uint32 classSymbol) noexcept {
    Compilation& c = compilation_;
    const Symbol classEntry = c.symbols_[classSymbol];
    if ((classEntry.flags & SymbolFlags::Incomplete) != 0 || classEntry.baseCount < 2) {
        return;
    }

    auto check = [&](uint32 interfaceSymbol, uint32 method) noexcept {
        const Symbol& required = c.symbols_[method];
        if ((required.flags & SymbolFlags::Erroneous) != 0) {
            return; // 그 메서드 선언의 진단이 이미 있다
        }

        const uint32 implementation = FindMemberInChain(classSymbol, required.name);
        if (implementation != NoSymbol &&
            (c.symbols_[implementation].flags & SymbolFlags::Erroneous) != 0) {
            return; // 구현 함수 이름의 진단이 이미 있다(선언 이름 하나에 하나)
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (implementation == NoSymbol || !IsFunctionSymbol(implementation)) {
            message.AppendFormat("Class '{}' does not implement '{}.{}'.", classEntry.name,
                                 c.symbols_[interfaceSymbol].name, required.name);
            if (Succeeded(message)) {
                Report(DiagnosticCode::InterfaceNotImplemented,
                       c.modules_[classEntry.module].source,
                       NodeSpan(classEntry.module, classEntry.nameNode), message.View(),
                       "A class implements every method of its interfaces with a function of "
                       "the same name and signature.",
                       "Add the function.");
            }

            return;
        }

        if (SameSignature(c.symbols_[implementation].signature, required.signature)) {
            return;
        }

        const Symbol& found = c.symbols_[implementation];
        const bool own = found.owner == classSymbol;
        message.AppendFormat("'{}' does not match the signature of '{}.{}'.", found.name,
                             c.symbols_[interfaceSymbol].name, required.name);
        if (Succeeded(message)) {
            Report(DiagnosticCode::InterfaceNotImplemented, c.modules_[classEntry.module].source,
                   own ? NodeSpan(found.module, found.nameNode)
                       : NodeSpan(classEntry.module, classEntry.nameNode),
                   message.View(),
                   "A class implements every method of its interfaces with a function of the "
                   "same name and signature.",
                   "Use the signature of the interface method.");
        }
    };

    // 기반 interface와 그 schema `base` 체인의 interface를 한 번씩 본다.
    Array<uint32> seen(allocator_);
    auto firstVisit = [&](uint32 symbol) noexcept {
        for (const uint32 visited : seen) {
            if (visited == symbol) {
                return false;
            }
        }

        if (auto pushed = seen.TryPushBack(symbol); !pushed) {
            error_ = pushed.Error();
            return false;
        }

        return true;
    };
    for (uint32 index = 1; index < classEntry.baseCount && error_ == ErrorCode::None; ++index) {
        uint32 interfaceSymbol = c.symbolBases_[classEntry.firstBase + index];
        if (c.symbols_[interfaceSymbol].kind == SymbolKind::Interface) {
            if (!firstVisit(interfaceSymbol)) {
                continue;
            }

            const Symbol& entry = c.symbols_[interfaceSymbol];
            const SyntaxTree& tree = c.trees_[entry.module];
            const uint32 methods = tree.Child(entry.node, 3);
            if (methods == NoNode || tree.Node(methods).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 method : tree.Children(methods)) {
                if (tree.Node(method).kind != SyntaxKind::InterfaceMethod) {
                    continue;
                }

                const uint32 symbol = c.NodeSymbol(entry.module, tree.Child(method, 2));
                if (symbol != NoSymbol) {
                    check(interfaceSymbol, symbol);
                }
            }

            continue;
        }

        // 엔진 interface와 그 `base` 체인의 함수들.
        for (uint32 step = 0; interfaceSymbol != NoSymbol && step < c.symbols_.Size(); ++step) {
            if (!firstVisit(interfaceSymbol)) {
                break;
            }

            const Symbol& entry = c.symbols_[interfaceSymbol];
            const BindingSchema& schema = c.schemas_[entry.index].schema;
            const SchemaType& type = schema.Types()[entry.item];
            for (uint32 function = type.firstFunction;
                 function < type.firstFunction + type.functionCount; ++function) {
                const uint32 symbol = FindName(SpaceKey(NameSpaceKind::Members, interfaceSymbol),
                                               schema.Text(schema.Functions()[function].name));
                if (symbol != NoSymbol) {
                    check(interfaceSymbol, symbol);
                }
            }

            interfaceSymbol = entry.base;
        }
    }
}

// class가 상속하는 멤버 이름이 그 모듈의 최상위 선언(S0310 — 그 선언에)·import한 이름(S0311 — 그
// import 줄에, 별칭이면 별칭에)과 철자가 같은지 본다. class 안에서 그 이름이 둘을 가리키지 않게.
void NameBinder::CheckInheritedNames(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint64 top = SpaceKey(NameSpaceKind::ModuleTop, module);
    const uint64 imports = SpaceKey(NameSpaceKind::ModuleImport, module);
    Array<uint32> reportedImports(allocator_);
    auto checkName = [&](uint32 classSymbol, uint32 baseSymbol, StringView name) noexcept {
        if (error_ != ErrorCode::None) {
            return;
        }

        const Symbol& classEntry = c.symbols_[classSymbol];
        const uint32 declared = FindName(top, name);
        if (declared != NoSymbol && (c.symbols_[declared].flags & SymbolFlags::Erroneous) == 0) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is also a member that class '{}' inherits from '{}'.", name,
                                 classEntry.name, c.symbols_[baseSymbol].name);
            if (Succeeded(message)) {
                ReportNameDiagnostic(DiagnosticCode::NameConflict, declared, message.View(),
                                     "Inside the class the name would mean both the inherited "
                                     "member and this declaration.",
                                     "Rename this declaration.");
            }

            return;
        }

        const uint32 imported = declared == NoSymbol ? FindName(imports, name) : NoSymbol;
        if (imported == NoSymbol || (c.symbols_[imported].flags & SymbolFlags::Erroneous) != 0) {
            return;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (c.symbols_[imported].kind == SymbolKind::Alias) {
            message.AppendFormat("Alias '{}' is also a member that class '{}' inherits from '{}'.",
                                 name, classEntry.name, c.symbols_[baseSymbol].name);
            if (Succeeded(message)) {
                ReportNameDiagnostic(DiagnosticCode::ImportConflict, imported, message.View(),
                                     "Inside the class the name would mean both the inherited "
                                     "member and the module.",
                                     "Choose another alias.");
            }

            return;
        }

        // 그 선언을 들여온(별칭 없는) 첫 import 줄이다.
        for (const ImportResolution& import : c.ModuleImports(module)) {
            const uint32 alias = tree.Child(import.node, 1);
            if (import.module != c.symbols_[imported].module ||
                (alias != NoNode && tree.Node(alias).kind == SyntaxKind::Name)) {
                continue;
            }

            for (const uint32 reported : reportedImports) {
                if (reported == import.node) {
                    return; // import 줄당 하나
                }
            }

            if (auto pushed = reportedImports.TryPushBack(import.node); !pushed) {
                error_ = pushed.Error();
                return;
            }

            message.AppendFormat("This import brings '{}', which class '{}' inherits from '{}'.",
                                 name, classEntry.name, c.symbols_[baseSymbol].name);
            if (Succeeded(message)) {
                const uint32 path = tree.Child(import.node, 0);
                Report(DiagnosticCode::ImportConflict, c.modules_[module].source,
                       SourceSpan{tree.Node(path).span.begin, tree.Node(path).span.begin},
                       message.View(),
                       "Inside the class the name would mean both the inherited member and the "
                       "imported declaration.",
                       "Import the module with an alias, as in 'import Weapons.Sword as "
                       "Blades;'.");
            }

            return;
        }
    };

    const uint32 declarations = tree.Child(tree.Root(), 1);
    for (const uint32 node : tree.Children(declarations)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        if (tree.Node(node).kind != SyntaxKind::Class) {
            continue;
        }

        const uint32 classSymbol = c.NodeSymbol(module, tree.Child(node, 2));
        if (classSymbol == NoSymbol ||
            (c.symbols_[classSymbol].flags & SymbolFlags::Incomplete) != 0) {
            continue;
        }

        for (uint32 base = c.symbols_[classSymbol].base, step = 0;
             base != NoSymbol && step < c.symbols_.Size() && error_ == ErrorCode::None;
             base = c.symbols_[base].base, ++step) {
            const Symbol& entry = c.symbols_[base];
            if (entry.kind == SymbolKind::Class) {
                const SyntaxTree& baseTree = c.trees_[entry.module];
                const uint32 members = baseTree.Child(entry.node, 4);
                if (members == NoNode || baseTree.Node(members).kind != SyntaxKind::List) {
                    continue;
                }

                for (const uint32 member : baseTree.Children(members)) {
                    const SyntaxNode& memberEntry = baseTree.Node(member);
                    if (memberEntry.kind == SyntaxKind::Handler || memberEntry.childCount < 3) {
                        continue;
                    }

                    const uint32 symbol = c.NodeSymbol(entry.module, baseTree.Child(member, 2));
                    if (symbol != NoSymbol) {
                        checkName(classSymbol, base, c.symbols_[symbol].name);
                    }
                }

                continue;
            }

            const BindingSchema& schema = c.schemas_[entry.index].schema;
            const SchemaType& type = schema.Types()[entry.item];
            for (uint32 field = type.firstField; field < type.firstField + type.fieldCount;
                 ++field) {
                checkName(classSymbol, base, schema.Text(schema.Fields()[field].name));
            }

            for (uint32 function = type.firstFunction;
                 function < type.firstFunction + type.functionCount; ++function) {
                checkName(classSymbol, base, schema.Text(schema.Functions()[function].name));
            }

            for (uint32 event = type.firstEvent; event < type.firstEvent + type.eventCount;
                 ++event) {
                checkName(classSymbol, base, schema.Text(schema.Events()[event].name));
            }
        }
    }
}

// manifest `services` 항목마다 같은 패키지의 WorldService 파생 최상위 class인지 본다(S0325).
void NameBinder::CheckServices() noexcept {
    Compilation& c = compilation_;
    for (uint32 package = 0; package < c.packages_.Size() && error_ == ErrorCode::None; ++package) {
        if (!c.packages_[package].valid) {
            continue;
        }

        const PackageManifest& manifest = c.packages_[package].manifest;
        for (const ManifestName& service : manifest.Services()) {
            const StringView text = manifest.Text(service);
            usize dot = text.ByteCount();
            while (dot > 0 && text.Data()[dot - 1] != '.') {
                --dot;
            }

            const StringView modulePath(text.Data(), dot - 1);
            const StringView className(text.Data() + dot, text.ByteCount() - dot);
            const uint32 module = c.FindModule(package, modulePath);
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            if (module == NoModule) {
                message.AppendFormat("Service '{}': this package has no module '{}'.", text,
                                     modulePath);
            } else if (c.nodeBase_[module] == NoNode || moduleStates_[module].syntaxErrors) {
                continue; // 그 모듈의 오류를 먼저 고친다
            } else {
                const uint32 symbol =
                    FindName(SpaceKey(NameSpaceKind::ModuleTop, module), className);
                if (symbol == NoSymbol || c.symbols_[symbol].kind != SymbolKind::Class) {
                    message.AppendFormat("Service '{}': module '{}' has no class '{}'.", text,
                                         modulePath, className);
                } else if ((c.symbols_[symbol].flags & SymbolFlags::Incomplete) != 0) {
                    continue;
                } else {
                    uint32 at = symbol;
                    while (at != NoSymbol && c.symbols_[at].kind == SymbolKind::Class) {
                        at = c.symbols_[at].base;
                    }

                    if (!EngineChainHas(at, worldService_)) {
                        message.AppendFormat("Service '{}': class '{}' is not a WorldService.",
                                             text, className);
                    }
                }
            }

            if (message.View().IsEmpty()) {
                continue;
            }

            if (Succeeded(message)) {
                Report(DiagnosticCode::InvalidService, c.packages_[package].source, service.span,
                       message.View(),
                       "A service is a top-level class derived from WorldService, named by its "
                       "module path and class name.",
                       "Write the module path and the class name, as in "
                       "'Combat.Score.ScoreKeeper'.");
            }
        }
    }
}

} // namespace rocks::sparks::detail
