#include <Core/Text/EditDistance.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "NameBinder.h"

// 본문 해석이다: 지역 이름의 범위와 검사(S0306·S0307·S0309·S0310), 식의 단순 이름(S0317·S0321),
// 타입 표기(S0318–S0320). 규칙: Docs/Design/SparksLanguage.ko.md 3.9절.

namespace rocks::sparks::detail {

namespace {

// 찾지 못한 이름에 비슷한 이름을 제안하는 편집 거리 문턱이다.
constexpr usize MaxNameSuggestionDistance = 2;

constexpr StringView RpcAttributeName = "rpc";
constexpr StringView ServerArgumentName = "Server";
constexpr uint32 CallerContextIndex = 4;

uint32 ContextIndex(StringView name) noexcept {
    for (uint32 index = 0; index < sizeof(ContextNames) / sizeof(ContextNames[0]); ++index) {
        if (ContextNames[index] == name) {
            return index;
        }
    }

    return NoSymbol;
}

} // namespace

// 찾는 차례로 대상을 넘겨받아, 선언 검사가 진단하지 않은 첫 대상을(모두 진단을 받았으면 첫 대상을)
// 고른다(3.9절 "찾기" — 충돌을 이미 알린 이름이 진단을 받지 않은 쪽을 가리키게).
class NameBinder::Pick final {
public:
    explicit Pick(const Compilation& compilation) noexcept : compilation_(compilation) {
    }

    void Offer(uint32 symbol) noexcept {
        if (symbol == NoSymbol) {
            return;
        }

        if (first_ == NoSymbol) {
            first_ = symbol;
        }

        if (clean_ == NoSymbol &&
            (compilation_.symbols_[symbol].flags & SymbolFlags::Erroneous) == 0) {
            clean_ = symbol;
        }
    }

    bool HasClean() const noexcept {
        return clean_ != NoSymbol;
    }

    bool HasAny() const noexcept {
        return first_ != NoSymbol;
    }

    uint32 Result() const noexcept {
        return clean_ != NoSymbol ? clean_ : first_;
    }

private:
    const Compilation& compilation_;
    uint32 first_ = NoSymbol;
    uint32 clean_ = NoSymbol;
};

uint32 NameBinder::LookupType(uint32 module, StringView name) const noexcept {
    const uint64 spaces[] = {
        SpaceKey(NameSpaceKind::ModuleTop, module), SpaceKey(NameSpaceKind::ModuleImport, module),
        SpaceKey(NameSpaceKind::Engine, 0), SpaceKey(NameSpaceKind::Language, 0)};
    Pick pick(compilation_);
    for (const uint64 space : spaces) {
        const uint32 found = FindName(space, name);
        if (found != NoSymbol && IsTypeSymbol(found)) {
            pick.Offer(found);
            if (pick.HasClean()) {
                break;
            }
        }
    }

    return pick.Result();
}

uint32 NameBinder::LookupValue(const BodyContext& context, StringView name,
                               bool* unavailableContext) const noexcept {
    if (unavailableContext != nullptr) {
        *unavailableContext = false;
    }

    Pick pick(compilation_);
    if (context.hasLocals) {
        pick.Offer(FindLocal(name));
    }

    if (context.owner != NoSymbol && !pick.HasClean()) {
        pick.Offer(FindName(SpaceKey(NameSpaceKind::Members, context.owner), name));
        if (!pick.HasClean()) {
            pick.Offer(FindInherited(context.owner, name, false));
        }
    }

    if (pick.HasClean()) {
        return pick.Result();
    }

    // 문맥 이름: 쓸 수 있으면 그것이고, 쓸 수 없으면 앞의 대상이 없을 때 "쓸 수 없음"이다(식의
    // `World`는 엔진 타입이 아니라 언제나 문맥 이름이다).
    const uint32 contextIndex = ContextIndex(name);
    if (contextIndex != NoSymbol) {
        if ((context.contextNames & (1u << contextIndex)) != 0) {
            pick.Offer(firstContextSymbol_ + contextIndex);
            return pick.Result();
        }

        if (!pick.HasAny() && unavailableContext != nullptr) {
            *unavailableContext = true;
        }

        return pick.Result();
    }

    const uint64 spaces[] = {SpaceKey(NameSpaceKind::ModuleTop, context.module),
                             SpaceKey(NameSpaceKind::ModuleImport, context.module),
                             SpaceKey(NameSpaceKind::Engine, 0),
                             SpaceKey(NameSpaceKind::Language, 0)};
    for (const uint64 space : spaces) {
        pick.Offer(FindName(space, name));
        if (pick.HasClean()) {
            break;
        }
    }

    return pick.Result();
}

uint32 NameBinder::ResolveSimpleName(const BodyContext& context, uint32 node, StringView name,
                                     SourceSpan span) noexcept {
    static_cast<void>(node);
    bool unavailableContext = false;
    const uint32 found = LookupValue(context, name, &unavailableContext);
    if (found != NoSymbol) {
        NoteUse(context.module, found);
        return found;
    }

    if (!unavailableContext) {
        unresolvedNames_[context.module] = 1;
    }

    if (unavailableContext) {
        if (context.reportContext) {
            const uint32 contextIndex = ContextIndex(name);
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("The context name '{}' is not available here.", name);
            const bool caller = contextIndex == CallerContextIndex;
            if (Succeeded(message)) {
                Report(DiagnosticCode::ContextNameUnavailable,
                       compilation_.modules_[context.module].source, span, message.View(),
                       caller ? StringView("'Caller' exists only in the body of a server RPC "
                                           "function of a class that has context names.")
                              : StringView("Context names exist in the functions and handlers of "
                                           "a class whose base type provides them: World, "
                                           "Session, Input and Time for components, actors and "
                                           "widget controllers, and World, Session and Time for "
                                           "world services. Struct functions, top-level "
                                           "functions and initial values have none."),
                       caller ? StringView("Use 'Caller' in an '@rpc(Server)' function.")
                              : StringView("Move the code into a function of a class that has "
                                           "this context name, or pass the value in."));
            }
        }

        return NoSymbol;
    }

    ReportUnknown(context, false, name, span);
    return NoSymbol;
}

// 찾지 못한 이름(S0317)·타입(S0318)이다. 수정 예: 별칭 import의 선언이면 `별칭.이름`, 같은 패키지와
// 의존 패키지에서 그 이름을 선언한 모듈이 하나면 그 import, 아니면 편집 거리 2 이하의 비슷한 이름.
void NameBinder::ReportUnknown(const BodyContext& context, bool type, StringView name,
                               SourceSpan span) noexcept {
    if (!context.reportUnknown || error_ != ErrorCode::None) {
        return;
    }

    Compilation& c = compilation_;
    const uint32 module = context.module;
    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
    // 별칭으로 import한 모듈의 선언.
    for (const ImportResolution& import : c.ModuleImports(module)) {
        if (import.module == NoModule || !fix.View().IsEmpty()) {
            continue;
        }

        const SyntaxTree& tree = c.trees_[module];
        const uint32 alias = tree.Child(import.node, 1);
        if (alias == NoNode || tree.Node(alias).kind != SyntaxKind::Name) {
            continue;
        }

        const uint32 found = FindName(SpaceKey(NameSpaceKind::ModuleTop, import.module), name);
        if (found != NoSymbol && (!type || IsTypeSymbol(found))) {
            fix.AppendFormat("Write '{}.{}'; the module is imported with an alias.",
                             NodeText(module, alias), name);
        }
    }

    // 같은 패키지·의존 패키지에서 그 이름을 선언한 모듈이 하나뿐이면 import를 제안한다.
    if (fix.View().IsEmpty()) {
        const uint32 package = c.modules_[module].package;
        uint32 candidate = NoSymbol;
        uint32 count = 0;
        auto consider = [&](uint32 owner) noexcept {
            const uint32* found = packageNames_.Find(NameKey{owner, name});
            if (found == nullptr) {
                return;
            }

            if (*found == AmbiguousSymbol) {
                count += 2;
                return;
            }

            if (c.symbols_[*found].module != module && (!type || IsTypeSymbol(*found))) {
                candidate = *found;
                ++count;
            }
        };
        consider(package);
        const PackageManifest& manifest = c.packages_[package].manifest;
        for (const ManifestName& dependency : manifest.Dependencies()) {
            const uint32 dependencyPackage = c.FindPackage(manifest.Text(dependency));
            if (dependencyPackage != NoPackage) {
                consider(dependencyPackage);
            }
        }

        if (count == 1) {
            const Symbol& symbol = c.symbols_[candidate];
            const uint32 targetPackage = c.modules_[symbol.module].package;
            if (targetPackage == package) {
                fix.AppendFormat("Add 'import {};'.", c.Text(c.modules_[symbol.module].importPath));
            } else {
                fix.AppendFormat("Add 'import {}.{};'.", c.packages_[targetPackage].manifest.Name(),
                                 c.Text(c.modules_[symbol.module].importPath));
            }
        }
    }

    // 비슷한 이름.
    if (fix.View().IsEmpty()) {
        StringView best;
        usize bestDistance = MaxNameSuggestionDistance + 1;
        auto consider = [&](StringView candidate, uint32 symbol) noexcept {
            if (type && !IsTypeSymbol(symbol)) {
                return;
            }

            const usize distance = EditDistance(name, candidate, MaxNameSuggestionDistance,
                                                EditDistanceCase::AsciiInsensitive);
            if (distance < bestDistance ||
                (distance == bestDistance && distance <= MaxNameSuggestionDistance &&
                 StringView::CompareBytes(candidate, best) < 0)) {
                bestDistance = distance;
                best = candidate;
            }
        };
        if (!type && context.hasLocals) {
            for (const LocalEntry& local : locals_) {
                consider(c.symbols_[local.symbol].name, local.symbol);
            }
        }

        // 보이는 이름 공간: 언어·엔진·모듈 최상위·import, 자기와 기반 체인의 멤버.
        Array<uint64> spaces(allocator_);
        const uint64 fixed[] = {SpaceKey(NameSpaceKind::Language, 0),
                                SpaceKey(NameSpaceKind::Engine, 0),
                                SpaceKey(NameSpaceKind::ModuleTop, module),
                                SpaceKey(NameSpaceKind::ModuleImport, module)};
        for (const uint64 space : fixed) {
            if (auto pushed = spaces.TryPushBack(space); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }

        if (!type) {
            for (uint32 at = context.owner, step = 0; at != NoSymbol && step < c.symbols_.Size();
                 at = c.symbols_[at].base, ++step) {
                if (auto pushed = spaces.TryPushBack(SpaceKey(NameSpaceKind::Members, at));
                    !pushed) {
                    error_ = pushed.Error();
                    return;
                }
            }
        }

        names_.ForEach([&](const NameKey& key, const uint32& symbol) noexcept {
            for (const uint64 space : spaces) {
                if (key.space == space) {
                    if (c.symbols_[symbol].kind == SymbolKind::ContextName &&
                        (context.contextNames & (1u << ContextIndex(key.name))) == 0) {
                        return;
                    }

                    consider(key.name, symbol);
                    return;
                }
            }
        });
        if (!best.IsEmpty()) {
            fix.AppendFormat("Did you mean '{}'?", best);
        }
    }

    if (fix.View().IsEmpty()) {
        fix.Append(type ? StringView("Declare the type or import the module that declares it.")
                        : StringView("Declare the name or import the module that declares it."));
    }

    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.AppendFormat(type ? StringView("Unknown type '{}'.") : StringView("Unknown name '{}'."),
                         name);
    if (Succeeded(message) && Succeeded(fix)) {
        Report(type ? DiagnosticCode::UnknownType : DiagnosticCode::UnknownName,
               c.modules_[module].source, span, message.View(),
               "Names come from this file, its imports, the members of the type, the engine and "
               "the prelude.",
               fix.View());
    }
}

uint32 NameBinder::ResolveTypePath(const BodyContext& context, uint32 path) noexcept {
    const uint32 symbol = LookupTypePath(context, path);
    if (symbol != NoSymbol) {
        NoteUse(context.module, symbol);
    } else {
        unresolvedNames_[context.module] = 1;
    }

    return symbol;
}

void NameBinder::NoteUse(uint32 module, uint32 symbol) noexcept {
    const Compilation& c = compilation_;
    const Symbol& entry = c.symbols_[symbol];
    uint32 target = NoModule;
    switch (entry.kind) {
    case SymbolKind::Alias:
        target = entry.index;
        break;
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
    case SymbolKind::Const:
    case SymbolKind::Function:
        target = entry.module;
        break;
    default:
        return; // 멤버·지역·엔진·언어 이름은 import로 찾지 않는다
    }

    if (target == module || target >= c.modules_.Size()) {
        return;
    }

    const auto& owner = c.modules_[module];
    for (uint32 index = 0; index < owner.importCount; ++index) {
        if (c.imports_[owner.firstImport + index].module == target) {
            importUsed_[owner.firstImport + index] = 1;
        }
    }
}

uint32 NameBinder::LookupTypePath(const BodyContext& context, uint32 path) noexcept {
    Compilation& c = compilation_;
    const uint32 module = context.module;
    const SyntaxTree& tree = c.trees_[module];
    if (path == NoNode || tree.Node(path).kind != SyntaxKind::Path) {
        return NoSymbol;
    }

    const Span<const uint32> segments = tree.Children(path);
    for (const uint32 segment : segments) {
        if (tree.Node(segment).kind != SyntaxKind::Name) {
            return NoSymbol; // 구문 오류(이미 알렸다)
        }
    }

    const SourceSpan firstSpan = tree.Node(segments[0]).span;
    const StringView first = NodeText(module, segments[0]);
    const SourceId source = c.modules_[module].source;
    if (segments.Size() == 1) {
        const uint32 symbol = LookupType(module, first);
        if (symbol != NoSymbol) {
            return symbol;
        }

        const uint32 language = FindName(SpaceKey(NameSpaceKind::Language, 0), first);
        if (language != NoSymbol && c.symbols_[language].kind == SymbolKind::PreludeType) {
            // 이름만 예약한 타입(`WorldPosition`).
            if (context.reportUnknown) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("Type '{}' is reserved for a future engine type.", first);
                if (Succeeded(message)) {
                    Report(DiagnosticCode::UnknownType, source, firstSpan, message.View(),
                           "The name is reserved, but the engine does not provide the type yet.",
                           "Use another type.");
                }
            }

            return NoSymbol;
        }

        // 타입이 아닌 대상(문맥 이름은 쓸 수 있는 자리와 무관하게 이름으로 본다).
        uint32 other = LookupValue(context, first);
        if (other == NoSymbol && ContextIndex(first) != NoSymbol) {
            other = firstContextSymbol_ + ContextIndex(first);
        }

        if (other != NoSymbol) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is not a type.", first);
            if (Succeeded(message)) {
                Report(DiagnosticCode::NotAType, source, firstSpan, message.View(),
                       "A type is a built-in or prelude type, an engine type, or a class, struct, "
                       "enum or interface.",
                       "Write the type of the value instead.");
            }

            return NoSymbol;
        }

        ReportUnknown(context, true, first, firstSpan);
        return NoSymbol;
    }

    const uint32 alias = FindName(SpaceKey(NameSpaceKind::ModuleImport, module), first);
    if (segments.Size() == 2 && alias != NoSymbol && c.symbols_[alias].kind == SymbolKind::Alias) {
        const uint32 target = c.symbols_[alias].index;
        const StringView second = NodeText(module, segments[1]);
        const SourceSpan secondSpan = tree.Node(segments[1]).span;
        const uint32 symbol = FindName(SpaceKey(NameSpaceKind::ModuleTop, target), second);
        if (symbol != NoSymbol && IsTypeSymbol(symbol)) {
            return symbol;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (symbol != NoSymbol) {
            message.AppendFormat("'{}.{}' is not a type.", first, second);
            if (Succeeded(message)) {
                Report(DiagnosticCode::NotAType, source, secondSpan, message.View(),
                       "A type is a built-in or prelude type, an engine type, or a class, struct, "
                       "enum or interface.",
                       "Write the type of the value instead.");
            }
        } else if (context.reportUnknown) {
            message.AppendFormat("Module '{}' has no type '{}'.",
                                 c.Text(c.modules_[target].importPath), second);
            if (Succeeded(message)) {
                Report(DiagnosticCode::UnknownType, source, secondSpan, message.View(),
                       "'Alias.Name' names a top-level declaration of the module imported with "
                       "that alias.",
                       "Check the name in that module.");
            }
        }

        return NoSymbol;
    }

    if (context.reportUnknown) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is not a type name.",
                             StringView(compilation_.sources_->Get(source).Text().Data() +
                                            tree.Node(path).span.begin,
                                        tree.Node(path).span.end - tree.Node(path).span.begin));
        if (Succeeded(message)) {
            Report(DiagnosticCode::UnknownType, source, firstSpan, message.View(),
                   "Imported declarations are used by their short name, and the declarations of "
                   "a module imported with an alias by 'Alias.Name'.",
                   "Write the short name, as in 'Sword', or import the module with an alias.");
        }
    }

    return NoSymbol;
}

uint32 NameBinder::ResolveTypeNode(const BodyContext& context, uint32 typeNode) noexcept {
    Compilation& c = compilation_;
    const uint32 module = context.module;
    const SyntaxTree& tree = c.trees_[module];
    if (typeNode == NoNode || tree.Node(typeNode).kind != SyntaxKind::Type) {
        return NoType;
    }

    const SyntaxNode& entry = tree.Node(typeNode);
    const uint32 symbol = ResolveTypePath(context, tree.Child(typeNode, 0));
    Array<uint32> arguments(allocator_);
    const uint32 list = tree.Child(typeNode, 1);
    usize argumentCount = 0;
    if (list != NoNode && tree.Node(list).kind == SyntaxKind::List) {
        for (const uint32 argument : tree.Children(list)) {
            ++argumentCount;
            const uint32 type = ResolveTypeNode(context, argument);
            if (auto pushed = arguments.TryPushBack(type); !pushed) {
                error_ = pushed.Error();
                return NoType;
            }
        }
    }

    uint32 type = NoType;
    if (symbol != NoSymbol) {
        const Symbol& target = c.symbols_[symbol];
        if (target.arity != argumentCount) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            if (target.arity == 0) {
                message.AppendFormat("Type '{}' does not take type arguments.", target.name);
                fix.AppendFormat("Write '{}'.", target.name);
            } else {
                message.AppendFormat("Type '{}' takes {} type argument{}.", target.name,
                                     target.arity, target.arity == 1 ? "" : "s");
                fix.Append(target.arity == 1 ? StringView("Write it as in 'Array<int>'.")
                                             : StringView("Write it as in 'Map<string, int>'."));
            }

            if (Succeeded(message) && Succeeded(fix)) {
                Report(DiagnosticCode::TypeArgumentCount, c.modules_[module].source,
                       SourceSpan{entry.span.begin, entry.span.begin}, message.View(), "",
                       fix.View());
            }
        } else {
            type = InternType(symbol, arguments.View(), (entry.flags & SyntaxFlags::Optional) != 0);
        }
    }

    SetNodeType(module, typeNode, type);
    return type;
}

void NameBinder::PushScope() noexcept {
    if (auto pushed = scopeMarks_.TryPushBack(static_cast<uint32>(locals_.Size())); !pushed) {
        error_ = pushed.Error();
    }
}

void NameBinder::PopScope() noexcept {
    if (scopeMarks_.IsEmpty()) {
        return;
    }

    const uint32 mark = scopeMarks_[scopeMarks_.Size() - 1];
    for (usize index = locals_.Size(); index > mark; --index) {
        const LocalEntry& entry = locals_[index - 1];
        const StringView name = compilation_.symbols_[entry.symbol].name;
        if (entry.previous == NoSymbol) {
            static_cast<void>(localIndex_.Erase(name));
        } else if (uint32* slot = localIndex_.Find(name)) {
            *slot = entry.previous;
        }
    }

    static_cast<void>(locals_.TryResize(mark));
    static_cast<void>(scopeMarks_.TryResize(scopeMarks_.Size() - 1));
}

uint32 NameBinder::FindLocal(StringView name) const noexcept {
    // 같은 철자가 여럿이면(중복·가림을 이미 진단했다) 진단을 받지 않은 쪽이다.
    Pick pick(compilation_);
    const uint32* index = localIndex_.Find(name);
    for (uint32 at = index != nullptr ? *index : NoSymbol; at != NoSymbol && !pick.HasClean();
         at = locals_[at].previous) {
        if (compilation_.symbols_[locals_[at].symbol].name == name) {
            pick.Offer(locals_[at].symbol);
        }
    }

    return pick.Result();
}

uint32 NameBinder::DeclareLocal(const BodyContext& context, SymbolKind kind, uint32 nameNode,
                                uint32 declaration, uint32 type, bool isLet) noexcept {
    Compilation& c = compilation_;
    const uint32 module = context.module;
    uint32 symbol = c.NodeSymbol(module, nameNode);
    if (symbol == NoSymbol) {
        Symbol entry;
        entry.kind = kind;
        entry.name = NodeText(module, nameNode);
        entry.module = module;
        entry.node = declaration;
        entry.nameNode = nameNode;
        entry.type = type;
        entry.flags = isLet ? SymbolFlags::Let : 0;
        symbol = AddSymbol(entry);
        if (symbol == NoSymbol) {
            return NoSymbol;
        }

        SetNodeSymbol(module, nameNode, symbol);
    }

    const StringView name = c.symbols_[symbol].name;
    const StringView what =
        kind == SymbolKind::Parameter ? StringView("Parameter name") : StringView("Local name");
    if (CheckDeclarationName(module, symbol, NameRule::Camel, what, false, NoSpace)) {
        const uint32* existing = localIndex_.Find(name);
        const uint32 mark = scopeMarks_.IsEmpty() ? 0 : scopeMarks_[scopeMarks_.Size() - 1];
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        DiagnosticCode code = DiagnosticCode::NameConflict;
        StringView explanation;
        if (existing != nullptr) {
            const Symbol& other = c.symbols_[locals_[*existing].symbol];
            if (*existing >= mark) {
                code = DiagnosticCode::DuplicateDeclaration;
                message.AppendFormat("'{}' is declared twice.", name);
                explanation = "Names in one block or parameter list are compared ignoring letter "
                              "case.";
            } else {
                message.AppendFormat("'{}' is already the name '{}' of an enclosing scope.", name,
                                     other.name);
                explanation = "A name in a nested block cannot hide a name of an enclosing "
                              "block, even in letter case only.";
            }
        } else {
            uint32 member = NoSymbol;
            if (context.owner != NoSymbol) {
                member = FindFolded(SpaceKey(NameSpaceKind::Members, context.owner), name);
                // 상속 멤버: Sparks 멤버는 대소문자 무시, 엔진 멤버는 정확히 같을 때만 —
                // 엔진 멤버 이름은 대문자로 시작하므로(schema 규칙) 소문자 지역 이름과는 겹치지
                // 않는다.
                if (member == NoSymbol && c.symbols_[context.owner].kind == SymbolKind::Class) {
                    member = FindInherited(context.owner, name, true);
                }
            }

            if (member != NoSymbol) {
                message.AppendFormat("'{}' differs from the member '{}' only in letter case.", name,
                                     c.symbols_[member].name);
                explanation = "Value names that differ only in letter case are too easy to "
                              "confuse.";
            } else {
                for (uint32 index = 0; index < sizeof(ContextNames) / sizeof(ContextNames[0]);
                     ++index) {
                    if ((context.contextNames & (1u << index)) != 0 &&
                        EqualsIgnoringAsciiCase(ContextNames[index], name)) {
                        message.AppendFormat(
                            "'{}' differs from the context name '{}' only in letter case.", name,
                            ContextNames[index]);
                        explanation = "Value names that differ only in letter case are too easy "
                                      "to confuse.";
                        break;
                    }
                }
            }
        }

        if (!message.View().IsEmpty() && Succeeded(message)) {
            ReportNameDiagnostic(code, symbol, message.View(), explanation, "Rename it.");
        }
    }

    // 진단과 무관하게 범위에 더한다(연쇄를 막는다).
    LocalEntry entry;
    entry.symbol = symbol;
    const uint32* existing = localIndex_.Find(name);
    entry.previous = existing != nullptr ? *existing : NoSymbol;
    if (auto pushed = locals_.TryPushBack(entry); !pushed) {
        error_ = pushed.Error();
        return symbol;
    }

    if (auto assigned = localIndex_.InsertOrAssign(name, static_cast<uint32>(locals_.Size() - 1));
        !assigned) {
        error_ = assigned.Error();
    }

    return symbol;
}

void NameBinder::BindBodies(uint32 module) noexcept {
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
        switch (entry.kind) {
        case SyntaxKind::Const:
            BindExpression(moduleContext, tree.Child(node, 4));
            break;
        case SyntaxKind::Function:
            BindFunctionBody(moduleContext, node);
            break;
        case SyntaxKind::Class:
        case SyntaxKind::Struct: {
            const bool isClass = entry.kind == SyntaxKind::Class;
            const uint32 owner = c.NodeSymbol(module, tree.Child(node, 2));
            BodyContext typeContext = moduleContext;
            typeContext.owner = owner;
            if (owner != NoSymbol && (c.symbols_[owner].flags & SymbolFlags::Incomplete) != 0) {
                typeContext.reportUnknown = false;
                typeContext.reportContext = false;
            }

            const uint32 members = tree.Child(node, isClass ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                break;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& memberEntry = tree.Node(member);
                if (memberEntry.kind == SyntaxKind::Field) {
                    BindExpression(typeContext, tree.Child(member, 4)); // 문맥 이름 없음
                } else if (memberEntry.kind == SyntaxKind::Function ||
                           memberEntry.kind == SyntaxKind::Handler) {
                    BodyContext functionContext = typeContext;
                    functionContext.contextNames = isClass ? ContextNamesOf(owner) : 0;
                    BindFunctionBody(functionContext, member);
                }
            }

            break;
        }
        default:
            break;
        }
    }
}

void NameBinder::BindFunctionBody(const BodyContext& outer, uint32 function) noexcept {
    Compilation& c = compilation_;
    const uint32 module = outer.module;
    const SyntaxTree& tree = c.trees_[module];
    const SyntaxNode& entry = tree.Node(function);
    const bool handler = entry.kind == SyntaxKind::Handler;
    BodyContext context = outer;
    context.hasLocals = true;
    // `Caller`: `@rpc`의 첫 인자가 이름 `Server`인 함수(구문 모양만 본다 — LC3).
    const uint32 attributes = tree.Child(function, 1);
    if (!handler && outer.contextNames != 0 && attributes != NoNode &&
        tree.Node(attributes).kind == SyntaxKind::List) {
        for (const uint32 attribute : tree.Children(attributes)) {
            if (tree.Node(attribute).kind != SyntaxKind::Attribute ||
                NodeText(module, attribute) != RpcAttributeName) {
                continue;
            }

            const uint32 arguments = tree.Child(attribute, 0);
            if (arguments == NoNode || tree.Node(arguments).childCount == 0) {
                continue;
            }

            const uint32 first = tree.Child(arguments, 0);
            const uint32 value =
                tree.Node(first).kind == SyntaxKind::Argument && tree.Child(first, 0) == NoNode
                    ? tree.Child(first, 1)
                    : NoNode;
            if (value != NoNode && tree.Node(value).kind == SyntaxKind::Identifier &&
                NodeText(module, value) == ServerArgumentName) {
                context.contextNames |= ContextBits::Caller;
            }
        }
    }

    if (handler) {
        const uint32 path = tree.Child(function, 2);
        // 이름 하나짜리 `on X()`는 hook이다(LC3). 경로의 첫 이름은 지역 이름 없이 찾는다.
        if (path != NoNode && tree.Node(path).kind == SyntaxKind::EventPath &&
            tree.Node(path).childCount >= 2) {
            BodyContext pathContext = outer;
            BindEventPath(pathContext, path);
        }
    }

    PushScope();
    const uint32 parameters = tree.Child(function, 3);
    if (parameters != NoNode && tree.Node(parameters).kind == SyntaxKind::List) {
        for (const uint32 parameter : tree.Children(parameters)) {
            if (tree.Node(parameter).kind != SyntaxKind::Parameter) {
                continue;
            }

            const uint32 nameNode = tree.Child(parameter, 0);
            uint32 type = NoType;
            if (handler) {
                type = ResolveTypeNode(context, tree.Child(parameter, 1));
            }

            if (nameNode != NoNode && tree.Node(nameNode).kind == SyntaxKind::Name) {
                DeclareLocal(context, SymbolKind::Parameter, nameNode, parameter, type, false);
            }
        }
    }

    BindStatement(context, tree.Child(function, handler ? 4 : 5));
    PopScope();
}

void NameBinder::BindEventPath(const BodyContext& context, uint32 path) noexcept {
    const SyntaxTree& tree = compilation_.trees_[context.module];
    if (path == NoNode || tree.Node(path).kind != SyntaxKind::EventPath ||
        tree.Node(path).childCount == 0) {
        return;
    }

    const uint32 first = tree.Child(path, 0);
    if (tree.Node(first).kind != SyntaxKind::Name) {
        return; // `this` 또는 Error
    }

    const uint32 symbol =
        ResolveSimpleName(context, first, NodeText(context.module, first), tree.Node(first).span);
    SetNodeSymbol(context.module, first, symbol);
}

void NameBinder::BindTypeArguments(const BodyContext& context, uint32 list) noexcept {
    const SyntaxTree& tree = compilation_.trees_[context.module];
    if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
        return;
    }

    for (const uint32 type : tree.Children(list)) {
        static_cast<void>(ResolveTypeNode(context, type));
    }
}

void NameBinder::BindStatement(const BodyContext& context, uint32 node) noexcept {
    const uint32 module = context.module;
    const SyntaxTree& tree = compilation_.trees_[module];
    if (node == NoNode || error_ != ErrorCode::None) {
        return;
    }

    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::Block:
        PushScope();
        for (const uint32 statement : tree.Children(node)) {
            BindStatement(context, statement);
        }

        PopScope();
        break;
    case SyntaxKind::LocalVariable: {
        const uint32 typeNode = tree.Child(node, 1);
        const uint32 type = typeNode == NoNode ? NoType : ResolveTypeNode(context, typeNode);
        BindExpression(context, tree.Child(node, 2));
        const uint32 exit = tree.Child(node, 3);
        if (exit != NoNode && tree.Node(exit).kind == SyntaxKind::ExitReturn) {
            BindExpression(context, tree.Child(exit, 0));
        }

        const uint32 nameNode = tree.Child(node, 0);
        if (nameNode != NoNode && tree.Node(nameNode).kind == SyntaxKind::Name) {
            DeclareLocal(context, SymbolKind::Local, nameNode, node, type,
                         (entry.flags & SyntaxFlags::Let) != 0);
        }

        break;
    }
    case SyntaxKind::If: {
        const uint32 condition = tree.Child(node, 0);
        const bool ifLet = condition != NoNode && tree.Node(condition).kind == SyntaxKind::IfLet;
        PushScope();
        if (ifLet) {
            BindExpression(context, tree.Child(condition, 1));
            const uint32 nameNode = tree.Child(condition, 0);
            if (nameNode != NoNode && tree.Node(nameNode).kind == SyntaxKind::Name) {
                DeclareLocal(context, SymbolKind::Local, nameNode, condition, NoType, true);
            }
        } else {
            BindExpression(context, condition);
        }

        BindStatement(context, tree.Child(node, 1));
        PopScope();
        BindStatement(context, tree.Child(node, 2));
        break;
    }
    case SyntaxKind::For: {
        BindExpression(context, tree.Child(node, 2));
        BindExpression(context, tree.Child(node, 3));
        PushScope();
        for (uint32 index = 0; index < 2; ++index) {
            const uint32 binder = tree.Child(node, index);
            if (binder != NoNode && tree.Node(binder).kind == SyntaxKind::Name) {
                DeclareLocal(context, SymbolKind::Local, binder, node, NoType, true);
            }
        }

        BindStatement(context, tree.Child(node, 4));
        PopScope();
        break;
    }
    case SyntaxKind::While:
        BindExpression(context, tree.Child(node, 0));
        BindStatement(context, tree.Child(node, 1));
        break;
    case SyntaxKind::Switch: {
        BindExpression(context, tree.Child(node, 0));
        const uint32 cases = tree.Child(node, 1);
        if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
            for (const uint32 item : tree.Children(cases)) {
                if (tree.Node(item).kind != SyntaxKind::SwitchCase) {
                    continue;
                }

                const uint32 values = tree.Child(item, 0);
                if (values != NoNode && tree.Node(values).kind == SyntaxKind::List) {
                    for (const uint32 value : tree.Children(values)) {
                        BindExpression(context, value);
                    }
                }

                BindStatement(context, tree.Child(item, 1));
            }
        }

        const uint32 fallback = tree.Child(node, 2);
        if (fallback != NoNode && tree.Node(fallback).kind == SyntaxKind::SwitchDefault) {
            BindStatement(context, tree.Child(fallback, 0));
        }

        break;
    }
    case SyntaxKind::Return:
    case SyntaxKind::DiscardAssignment:
    case SyntaxKind::ExpressionStatement:
        BindExpression(context, entry.childCount > 0 ? tree.Child(node, 0) : NoNode);
        break;
    case SyntaxKind::Assignment:
        BindExpression(context, tree.Child(node, 0));
        BindExpression(context, tree.Child(node, 1));
        break;
    case SyntaxKind::Emit: {
        BindEventPath(context, tree.Child(node, 0));
        const uint32 arguments = tree.Child(node, 1);
        if (arguments != NoNode && tree.Node(arguments).kind == SyntaxKind::List) {
            for (const uint32 argument : tree.Children(arguments)) {
                BindExpression(context, argument);
            }
        }

        break;
    }
    default:
        break; // break·continue·Error
    }
}

void NameBinder::BindExpression(const BodyContext& context, uint32 node) noexcept {
    const uint32 module = context.module;
    const SyntaxTree& tree = compilation_.trees_[module];
    if (node == NoNode || error_ != ErrorCode::None) {
        return;
    }

    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::Identifier: {
        const StringView name = NodeText(module, node);
        const SourceSpan span = tree.Tokens()[entry.token].span;
        SetNodeSymbol(module, node, ResolveSimpleName(context, node, name, span));
        BindTypeArguments(context, tree.Child(node, 0));
        break;
    }
    case SyntaxKind::Member:
        BindExpression(context, tree.Child(node, 0));
        BindTypeArguments(context, tree.Child(node, 1));
        break;
    case SyntaxKind::Call: {
        BindExpression(context, tree.Child(node, 0));
        const uint32 arguments = tree.Child(node, 1);
        if (arguments != NoNode && tree.Node(arguments).kind == SyntaxKind::List) {
            for (const uint32 argument : tree.Children(arguments)) {
                if (tree.Node(argument).kind == SyntaxKind::Argument) {
                    BindExpression(context, tree.Child(argument, 1)); // 이름 지정 인자 이름은 LC2c
                }
            }
        }

        break;
    }
    case SyntaxKind::CastTest: {
        BindExpression(context, tree.Child(node, 0));
        const uint32 path = tree.Child(node, 1);
        const uint32 target = ResolveTypePath(context, path);
        SetNodeType(module, path, InternType(target, Span<const uint32>(), false));
        break;
    }
    case SyntaxKind::Index:
    case SyntaxKind::Binary:
    case SyntaxKind::Unary:
    case SyntaxKind::Try:
    case SyntaxKind::Await:
    case SyntaxKind::Parenthesized:
    case SyntaxKind::Interpolation:
    case SyntaxKind::String:
    case SyntaxKind::CollectionLiteral:
    case SyntaxKind::MapEntry:
        for (const uint32 child : tree.Children(node)) {
            BindExpression(context, child);
        }

        break;
    default:
        break; // 리터럴·this·StringText·Error
    }
}

} // namespace rocks::sparks::detail
