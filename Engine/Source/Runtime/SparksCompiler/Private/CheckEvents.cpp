#include <Core/Text/EditDistance.h>

#include "CompilationChecker.h"
#include "TypeChecker.h"

// 이벤트·hook 선언 규칙이다(Docs/Design/SparksLanguage.ko.md 3.12절): hook 이름·인자(S0803–S0805),
// handler 자리(S0804), 중복(S0806), `on` 경로 해석(S0807–S0810), `emit`(S0811), 이벤트 호출
// (S0812), 확장 이벤트(S0813–S0815), 재진입 표시(S0816·S0817), LevelScript(S0801). Rules 단계
// (LC3)에서만 돈다.

namespace rocks::sparks::detail {

namespace {

// hook의 인자 모양이다.
enum class HookShape : uint8 {
    Plain,     // 인자 없음
    Tick,      // (dt: float)
    Trigger,   // (other: Actor, trigger: Collider)
    Collision, // (other: Actor, hit: Hit)
};

struct HookEntry {
    StringView name;
    HookShape shape;
};

// §5.1의 hook이다(`Validate`는 full — 프로필은 S1113이 본다).
constexpr HookEntry HookEntries[] = {
    {"Awake", HookShape::Plain},
    {"Enable", HookShape::Plain},
    {"Start", HookShape::Plain},
    {"Tick", HookShape::Tick},
    {"FixedTick", HookShape::Tick},
    {"LateTick", HookShape::Tick},
    {"Disable", HookShape::Plain},
    {"Destroy", HookShape::Plain},
    {"TriggerEnter", HookShape::Trigger},
    {"TriggerExit", HookShape::Trigger},
    {"CollisionEnter", HookShape::Collision},
    {"CollisionExit", HookShape::Collision},
    {"Validate", HookShape::Plain},
};

// 재진입 표시 attribute 이름이다(SD11).
constexpr StringView SkipWhileBusyAttribute = "skipWhileBusy";
constexpr StringView RestartAttribute = "restart";

// 예약 이벤트 이름이다(§5.2).
constexpr StringView ChangedEventName = "Changed";

// hook 이름 제안의 편집 거리 상한이다.
constexpr usize MaxHookSuggestionDistance = 2;

} // namespace

uint32 TypeChecker::EngineSymbol(StringView name) const noexcept {
    return binder_.FindName(SpaceKey(NameSpaceKind::Engine, 0), name);
}

bool TypeChecker::ContainsAwait(uint32 module, uint32 node) noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    if (node == NoNode) {
        return false;
    }

    Array<uint32> pending(allocator_);
    if (auto pushed = pending.TryPushBack(node); !pushed) {
        binder_.error_ = pushed.Error();
        return false;
    }

    while (!pending.IsEmpty()) {
        const uint32 current = pending.TryPopBack().Value(); // 비어 있지 않다
        const SyntaxNode& entry = tree.Node(current);
        if (entry.kind == SyntaxKind::Await ||
            (entry.kind == SyntaxKind::Try && (entry.flags & SyntaxFlags::Await) != 0)) {
            return true;
        }

        for (const uint32 child : tree.Children(current)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                binder_.error_ = pushed.Error();
                return false;
            }
        }
    }

    return false;
}

bool TypeChecker::HasChanged(uint32 field) const noexcept {
    const Symbol& entry = compilation_.symbols_[field];
    if (entry.kind == SymbolKind::Field) {
        return (entry.flags & SymbolFlags::Let) == 0;
    }

    if (entry.kind == SymbolKind::EngineField) {
        return compilation_.schemas_[entry.index].schema.Fields()[entry.item].changed;
    }

    return false;
}

uint32 TypeChecker::FindExtensionEvent(uint32 module, uint32 target, StringView name,
                                       uint32* hiddenModule) const noexcept {
    const Compilation& c = compilation_;
    const Span<const ImportResolution> imports = c.ModuleImports(module);
    for (const uint32 event : extensionEvents_) {
        const Symbol& entry = c.symbols_[event];
        if (entry.owner != target || entry.name != name) {
            continue;
        }

        bool visible = entry.module == module;
        for (const ImportResolution& import : imports) {
            visible = visible || import.module == entry.module;
        }

        if (visible) {
            return event;
        }

        if (hiddenModule != nullptr && *hiddenModule == NoModule) {
            *hiddenModule = entry.module;
        }
    }

    return NoSymbol;
}

uint32 TypeChecker::FindEvent(uint32 module, uint32 type, StringView name,
                              uint32* hiddenModule) noexcept {
    if (type == NoType) {
        return NoSymbol;
    }

    const uint32 member = FindMember(NonOptional(type), name);
    if (member != NoSymbol) {
        const SymbolKind kind = compilation_.symbols_[member].kind;
        if (kind == SymbolKind::Event || kind == SymbolKind::EngineEvent ||
            kind == SymbolKind::LanguageEvent) {
            return member;
        }
    }

    const uint32 symbol = SymbolOf(NonOptional(type));
    uint32 target = NoSymbol;
    if (binder_.EngineChainHas(symbol, binder_.actor_)) {
        target = binder_.actor_;
    } else if (symbol != NoSymbol && symbol == EngineSymbol("World")) {
        target = symbol;
    }

    if (target == NoSymbol) {
        return NoSymbol;
    }

    const uint32 extension = FindExtensionEvent(module, target, name, hiddenModule);
    if (extension != NoSymbol) {
        binder_.NoteUse(module, extension);
    }

    return extension;
}

void TypeChecker::CheckHandlerParameters(const CheckContext& context, uint32 handler,
                                         StringView eventName, Span<const uint32> expected,
                                         DiagnosticCode code) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 parameters = tree.Child(handler, 3);
    if (parameters == NoNode || tree.Node(parameters).kind != SyntaxKind::List) {
        return;
    }

    usize index = 0;
    for (const uint32 parameter : tree.Children(parameters)) {
        if (tree.Node(parameter).kind != SyntaxKind::Parameter) {
            continue;
        }

        const uint32 nameNode = tree.Child(parameter, 0);
        const uint32 typeNode = tree.Child(parameter, 1);
        if (index >= expected.Size()) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' passes {} argument{}.", eventName, expected.Size(),
                                 expected.Size() == 1 ? "" : "s");
            if (binder_.Succeeded(message)) {
                Report(context, code, nameNode != NoNode ? nameNode : parameter, message.View(),
                       "A handler declares the arguments it needs from the front of the list.",
                       "Remove the extra parameters.");
            }

            return;
        }

        const uint32 type = typeNode == NoNode ? NoType : c.NodeType(context.module, typeNode);
        if (type != NoType && expected[index] != NoType && type != expected[index]) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Parameter {} of '{}' has the type '", index + 1, eventName);
            AppendDisplayType(message, expected[index]);
            message.Append("'.");
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            fix.Append("Write '");
            AppendDisplayType(fix, expected[index]);
            fix.Append("'.");
            if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
                Report(context, code, typeNode, message.View(),
                       "Handler parameter types match the arguments exactly.", fix.View());
            }
        }

        ++index;
    }
}

void TypeChecker::CheckReentrancy(const CheckContext& context, uint32 node, bool allowed,
                                  bool awaits, uint32 reportNode) noexcept {
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 attributes = tree.Child(node, 1);
    uint32 count = 0;
    if (attributes != NoNode && tree.Node(attributes).kind == SyntaxKind::List) {
        for (const uint32 attribute : tree.Children(attributes)) {
            if (tree.Node(attribute).kind != SyntaxKind::Attribute) {
                continue;
            }

            const StringView name = TokenText(context.module, attribute);
            if (name != SkipWhileBusyAttribute && name != RestartAttribute) {
                continue;
            }

            ++count;
            if (count > 1) {
                Report(context, DiagnosticCode::MisplacedReentrancyPolicy, attribute,
                       "A handler has one reentrancy policy.",
                       "'@skipWhileBusy' drops new events while it waits; '@restart' cancels and "
                       "starts again.",
                       "Keep one of them.");
            } else if (!allowed) {
                Report(context, DiagnosticCode::MisplacedReentrancyPolicy, attribute,
                       "A reentrancy policy is only for event handlers and physics hooks.",
                       "Lifecycle hooks, 'Changed' handlers and functions are never re-entered.",
                       "Remove the attribute.");
            } else if (!awaits) {
                Report(context, DiagnosticCode::MisplacedReentrancyPolicy, attribute,
                       "This handler does not wait, so it has no reentrancy policy.",
                       "A handler that does not 'await' always finishes before the next event.",
                       "Remove the attribute.");
            }
        }
    }

    if (allowed && awaits && count == 0) {
        Report(context, DiagnosticCode::MissingReentrancyPolicy, reportNode,
               "A handler that waits needs a reentrancy policy.",
               "While it waits, the same source can raise the event again.",
               "Add '@skipWhileBusy' to drop those events, or '@restart' to start again.");
    }
}

void TypeChecker::CheckHook(const CheckContext& context, uint32 handler, uint32 nameNode,
                            Array<StringView>& seen) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const StringView name = TokenText(context.module, nameNode);
    const HookEntry* hook = nullptr;
    for (const HookEntry& entry : HookEntries) {
        if (entry.name == name) {
            hook = &entry;
        }
    }

    if (hook == nullptr) {
        // 자기 class의 이벤트를 hook 형태로 받으려 했는지, 비슷한 hook 이름이 있는지 본다.
        const uint32 member =
            context.owner == NoSymbol ? NoSymbol : binder_.FindMemberInChain(context.owner, name);
        if (member != NoSymbol && c.symbols_[member].kind == SymbolKind::Event) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is an event of this class, not a hook.", name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::UnknownHook, nameNode, message.View(),
                       "A class does not handle its own events; it already knows when it raises "
                       "them.",
                       "Call the function you need where the event is raised.");
            }

            return;
        }

        StringView best;
        usize bestDistance = MaxHookSuggestionDistance + 1;
        for (const HookEntry& entry : HookEntries) {
            const usize distance = EditDistance(name, entry.name, MaxHookSuggestionDistance,
                                                EditDistanceCase::AsciiInsensitive);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = entry.name;
            }
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is not a lifecycle hook.", name);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        if (best.IsEmpty()) {
            fix.Append("Use a hook such as 'on Start()', or name the event source, as in "
                       "'on Owner.Destroyed()'.");
        } else {
            fix.AppendFormat("Write 'on {}(...)'.", best);
        }

        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            Report(context, DiagnosticCode::UnknownHook, nameNode, message.View(),
                   "'on X()' with one name is a lifecycle hook; events are handled as "
                   "'on Source.Event()'.",
                   fix.View());
        }

        return;
    }

    const bool physics = hook->shape == HookShape::Trigger || hook->shape == HookShape::Collision;
    if (physics && binder_.EngineChainHas(context.owner, EngineSymbol("LevelScript"))) {
        Report(context, DiagnosticCode::LevelScriptPhysicsHook, handler,
               "A LevelScript cannot declare physics hooks.",
               "A LevelScript has no colliders. Place a trigger actor and handle its event.",
               "Handle the trigger actor's event, as in 'on GateTrigger.TriggerEntered(other: "
               "Actor) { ... }'.");
        return;
    }

    if (physics && !binder_.EngineChainHas(context.owner, binder_.component_) &&
        !binder_.EngineChainHas(context.owner, binder_.actor_)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is only for components and actors.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::HookUnavailable, nameNode, message.View(),
                   "Physics hooks come from the colliders of the object's own actor.",
                   "Handle an actor's event instead, as in 'on Gate.TriggerEntered(...)'.");
        }

        return;
    }

    for (const StringView previous : seen) {
        if (previous == name) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Hook '{}' is declared twice in this class.", name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::DuplicateHandler, nameNode, message.View(),
                       "A class declares each hook once; base and derived classes add up.",
                       "Merge the two hooks.");
            }

            return;
        }
    }

    if (auto pushed = seen.TryPushBack(name); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    uint32 expected[2] = {NoType, NoType};
    usize expectedCount = 0;
    switch (hook->shape) {
    case HookShape::Tick:
        expected[0] = floatType_;
        expectedCount = 1;
        break;
    case HookShape::Trigger:
        expected[0] = TypeOfSymbol(binder_.actor_);
        expected[1] = TypeOfSymbol(EngineSymbol("Collider"));
        expectedCount = 2;
        break;
    case HookShape::Collision:
        expected[0] = TypeOfSymbol(binder_.actor_);
        expected[1] = TypeOfSymbol(EngineSymbol("Hit"));
        expectedCount = 2;
        break;
    case HookShape::Plain:
        break;
    }

    CheckHandlerParameters(context, handler, name, Span<const uint32>(expected, expectedCount),
                           DiagnosticCode::HookSignature);
    CheckReentrancy(context, handler, physics,
                    ContainsAwait(context.module, tree.Child(handler, 4)), nameNode);
}

void TypeChecker::CheckPathHandler(const CheckContext& context, uint32 handler) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 path = tree.Child(handler, 2);
    const Span<const uint32> segments = tree.Children(path);
    const usize count = segments.Size();
    const uint32 last = segments[count - 1];
    if (tree.Node(last).kind != SyntaxKind::Name) {
        return; // 구문 오류
    }

    const StringView name = TokenText(context.module, last);
    const uint32 first = segments[0];
    auto unknownSource = [&](uint32 at, StringView message, StringView explanation) noexcept {
        Report(context, DiagnosticCode::UnknownEventSource, at, message, explanation,
               "Use a field, 'Owner', 'World', or 'this' in an actor class.");
    };
    uint32 event = NoSymbol;
    uint32 hidden = NoModule;
    uint32 element = NoType; // 배열 원천의 원소 타입
    bool changed = false;
    if (tree.Node(first).kind == SyntaxKind::This) {
        if (!binder_.EngineChainHas(context.owner, binder_.actor_)) {
            unknownSource(first, "'this' has events only in an actor class.",
                          "A component handles its actor's events through 'Owner'.");
            return;
        }

        if (count != 2) {
            unknownSource(segments[1], "Write 'on this.Event()'.",
                          "An event path names a source and one event.");
            return;
        }

        event = FindEvent(context.module, TypeOfSymbol(context.owner), name, &hidden);
    } else if (tree.Node(first).kind == SyntaxKind::Name) {
        const uint32 symbol = c.NodeSymbol(context.module, first);
        if (symbol == NoSymbol) {
            return; // 이름 해석이 알렸다
        }

        const Symbol& entry = c.symbols_[symbol];
        const uint32 widgetController = binder_.widgetController_;
        if (entry.kind == SymbolKind::ContextName && entry.name == "World") {
            if (count != 2) {
                unknownSource(segments[1], "Write 'on World.Event()'.",
                              "An event path names a source and one event.");
                return;
            }

            event = FindEvent(context.module, TypeOfSymbol(EngineSymbol("World")), name, &hidden);
        } else if (entry.kind == SymbolKind::EngineField && entry.name == "Widget" &&
                   binder_.EngineChainHas(entry.owner, widgetController)) {
            if (count != 3) {
                unknownSource(segments[1], "Write 'on Widget.Name.Event()'.",
                              "A widget event names one child widget of the widget asset.");
            }

            return; // widget 이름·이벤트는 WidgetAsset schema와 대조한다(연동 단계)
        } else if (entry.kind == SymbolKind::Field || entry.kind == SymbolKind::EngineField) {
            const uint32 fieldType =
                entry.kind == SymbolKind::Field ? FieldType(symbol) : entry.type;
            if (fieldType == NoType) {
                return;
            }

            if (count == 2 && name == ChangedEventName) {
                if (!HasChanged(symbol)) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.AppendFormat("Field '{}' has no 'Changed' event.", entry.name);
                    if (binder_.Succeeded(message)) {
                        Report(context, DiagnosticCode::ChangedUnavailable, last, message.View(),
                               "'Changed' exists for 'var' fields of scripts and engine fields "
                               "marked as changed in the binding schema.",
                               "Handle a field declared with 'var'.");
                    }

                    return;
                }

                changed = true;
            } else if (count == 2) {
                uint32 type = NonOptional(fieldType);
                if (IsLanguage(type, "Array")) {
                    element = Argument(type, 0);
                    type = element;
                }

                event = FindEvent(context.module, type, name, &hidden);
            } else if (count == 3 && name == ChangedEventName) {
                const uint32 inner =
                    tree.Node(segments[1]).kind == SyntaxKind::Name
                        ? FindMember(NonOptional(fieldType), TokenText(context.module, segments[1]))
                        : NoSymbol;
                const SymbolKind innerKind =
                    inner == NoSymbol ? SymbolKind::BuiltinType : c.symbols_[inner].kind;
                if (!IsObjectType(NonOptional(fieldType)) ||
                    (innerKind != SymbolKind::Field && innerKind != SymbolKind::EngineField)) {
                    unknownSource(segments[1], "This is not a field of the object.",
                                  "'on A.B.Changed()' names a field B of the object in field A.");
                    return;
                }

                if (!HasChanged(inner)) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.AppendFormat("Field '{}' has no 'Changed' event.",
                                         c.symbols_[inner].name);
                    if (binder_.Succeeded(message)) {
                        Report(context, DiagnosticCode::ChangedUnavailable, last, message.View(),
                               "'Changed' exists for 'var' fields of scripts and engine fields "
                               "marked as changed in the binding schema.",
                               "Handle a field declared with 'var'.");
                    }

                    return;
                }

                changed = true;
            } else {
                unknownSource(segments[1], "An event path has one source before the event.",
                              "Write 'on Field.Event()' or 'on Field.Value.Changed()'.");
                return;
            }
        } else {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is not an event source.", entry.name);
            if (binder_.Succeeded(message)) {
                unknownSource(first, message.View(),
                              "An event source is a field, 'Owner', 'World', or 'this' in an "
                              "actor class.");
            }

            return;
        }
    } else {
        return; // 구문 오류
    }

    if (!changed && event == NoSymbol) {
        if (hidden != NoModule) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Extension event '{}' is declared in module '{}', which is not "
                                 "imported here.",
                                 name, c.Text(c.modules_[hidden].importPath));
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            fix.AppendFormat("Add 'import {};'.", c.Text(c.modules_[hidden].importPath));
            if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
                Report(context, DiagnosticCode::ExtensionEventNotImported, last, message.View(),
                       "Extension events are used only from modules that import their module "
                       "directly.",
                       fix.View());
            }

            return;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("No event '{}' here.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::UnknownEvent, last, message.View(),
                   "Events come from the class or engine type of the source, and actor and World "
                   "extension events.",
                   "Check the event name.");
        }

        return;
    }

    // 인자: 배열 원천은 원소 하나가 앞에 온다. Changed는 인자가 없다.
    Array<uint32> expected(allocator_);
    if (element != NoType) {
        if (auto pushed = expected.TryPushBack(element); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }

    if (!changed) {
        const uint32 signature = c.symbols_[event].signature;
        if (signature != NoSignature) {
            const Signature& entry = c.signatures_[signature];
            for (uint32 index = 0; index < entry.parameterCount; ++index) {
                if (auto pushed =
                        expected.TryPushBack(c.signatureParameters_[entry.firstParameter + index]);
                    !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    CheckHandlerParameters(context, handler, name, expected.View(),
                           DiagnosticCode::HandlerSignature);
    CheckReentrancy(context, handler, !changed,
                    ContainsAwait(context.module, tree.Child(handler, 4)), path);
}

bool TypeChecker::SamePath(uint32 leftModule, uint32 left, uint32 rightModule,
                           uint32 right) const noexcept {
    const SyntaxTree& leftTree = compilation_.trees_[leftModule];
    const SyntaxTree& rightTree = compilation_.trees_[rightModule];
    const Span<const uint32> a = leftTree.Children(left);
    const Span<const uint32> b = rightTree.Children(right);
    if (a.Size() != b.Size()) {
        return false;
    }

    for (usize index = 0; index < a.Size(); ++index) {
        const SyntaxKind kind = leftTree.Node(a[index]).kind;
        if (kind != rightTree.Node(b[index]).kind) {
            return false;
        }

        if (kind == SyntaxKind::Name &&
            TokenText(leftModule, a[index]) != TokenText(rightModule, b[index])) {
            return false;
        }
    }

    return true;
}

void TypeChecker::CheckHandlers(const CheckContext& context, uint32 typeNode) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const bool isClass = tree.Node(typeNode).kind == SyntaxKind::Class;
    const uint32 members = tree.Child(typeNode, isClass ? 4 : 3);
    if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
        return;
    }

    const bool lifecycle = isClass && context.owner != NoSymbol &&
                           (c.symbols_[context.owner].flags & SymbolFlags::Lifecycle) != 0;
    Array<StringView> hooks(allocator_);
    for (const uint32 member : tree.Children(members)) {
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(member);
        if (entry.kind == SyntaxKind::Function) {
            CheckReentrancy(context, member, false, false, NoNode);
            continue;
        }

        if (entry.kind == SyntaxKind::Event) {
            const uint32 nameNode = tree.Child(member, 2);
            if (nameNode != NoNode && TokenText(context.module, nameNode) == ChangedEventName) {
                Report(context, DiagnosticCode::ChangedUnavailable, nameNode,
                       "'Changed' is a reserved event name.",
                       "'X.Changed' always means a change of field X.",
                       "Choose another event name.");
            }

            continue;
        }

        if (entry.kind != SyntaxKind::Handler) {
            continue;
        }

        const uint32 path = tree.Child(member, 2);
        if (path == NoNode || tree.Node(path).kind != SyntaxKind::EventPath ||
            tree.Node(path).childCount == 0) {
            continue;
        }

        if (!lifecycle) {
            Report(context, DiagnosticCode::HookUnavailable, path,
                   "Hooks and event handlers are only in lifecycle classes.",
                   "Components, actors, world services and widget controllers receive hooks "
                   "and events.",
                   "Derive the class from 'ScriptableComponent' or another lifecycle type.");
            continue;
        }

        const uint32 first = tree.Child(path, 0);
        if (tree.Node(path).childCount == 1 && tree.Node(first).kind == SyntaxKind::Name) {
            CheckHook(context, member, first, hooks);
        } else {
            CheckPathHandler(context, member);
        }
    }

    if (!lifecycle) {
        return;
    }

    // 같은 경로를 class 계층에서 둘 선언하면 나중 것이 S0806이다(기반 체인 → 이 class 순).
    struct Declared {
        uint32 module;
        uint32 path;
    };
    Array<Declared> earlier(allocator_);
    Array<uint32> chain(allocator_);
    uint32 step = 0;
    for (uint32 base = c.symbols_[context.owner].base;
         base != NoSymbol && c.symbols_[base].kind == SymbolKind::Class && step < c.symbols_.Size();
         base = c.symbols_[base].base, ++step) {
        if (auto pushed = chain.TryPushBack(base); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }

    for (usize index = chain.Size(); index > 0; --index) {
        const Symbol& base = c.symbols_[chain[index - 1]];
        const SyntaxTree& baseTree = c.trees_[base.module];
        const uint32 baseMembers = baseTree.Child(base.node, 4);
        if (baseMembers == NoNode || baseTree.Node(baseMembers).kind != SyntaxKind::List) {
            continue;
        }

        for (const uint32 member : baseTree.Children(baseMembers)) {
            const uint32 path = baseTree.Node(member).kind == SyntaxKind::Handler
                                    ? baseTree.Child(member, 2)
                                    : NoNode;
            if (path != NoNode && baseTree.Node(path).kind == SyntaxKind::EventPath &&
                baseTree.Node(path).childCount > 1) {
                if (auto pushed = earlier.TryPushBack(Declared{base.module, path}); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    for (const uint32 member : tree.Children(members)) {
        const uint32 path =
            tree.Node(member).kind == SyntaxKind::Handler ? tree.Child(member, 2) : NoNode;
        if (path == NoNode || tree.Node(path).kind != SyntaxKind::EventPath ||
            tree.Node(path).childCount < 2) {
            continue;
        }

        bool duplicate = false;
        for (const Declared& previous : earlier) {
            duplicate = duplicate || SamePath(previous.module, previous.path, context.module, path);
        }

        if (duplicate) {
            Report(context, DiagnosticCode::DuplicateHandler, path,
                   "This event is already handled in this class or a base class.",
                   "An event path is handled once in a class hierarchy; derived classes change "
                   "the behavior by overriding a function that the handler calls.",
                   "Remove this handler, or override the function that the base handler calls.");
        }

        if (auto pushed = earlier.TryPushBack(Declared{context.module, path}); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }
}

uint32 TypeChecker::ResolveEmit(const CheckContext& context, uint32 node) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 path = tree.Child(node, 0);
    if (path == NoNode || tree.Node(path).kind != SyntaxKind::EventPath ||
        tree.Node(path).childCount == 0) {
        return NoSignature;
    }

    const Span<const uint32> segments = tree.Children(path);
    const uint32 first = segments[0];
    if (segments.Size() == 1) {
        if (tree.Node(first).kind != SyntaxKind::Name) {
            return NoSignature;
        }

        const uint32 symbol = c.NodeSymbol(context.module, first);
        if (symbol == NoSymbol) {
            return NoSignature; // 이름 해석이 알렸다
        }

        const Symbol& entry = c.symbols_[symbol];
        if (entry.kind == SymbolKind::Event && entry.owner == context.owner) {
            return entry.signature;
        }

        if (entry.kind == SymbolKind::Event || entry.kind == SymbolKind::EngineEvent) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is not declared in this class, so it cannot be raised here.",
                                 entry.name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::EmitNotAllowed, first, message.View(),
                       "An event is raised only in the class that declares it; engine events "
                       "are raised by the engine.",
                       "Call a function of the declaring class that raises it.");
            }

            return NoSignature;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' is not an event.", entry.name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::UnknownEvent, first, message.View(),
                   "'emit' raises an event declared with 'event'.", "Check the event name.");
        }

        return NoSignature;
    }

    if (segments.Size() > 2) {
        Report(context, DiagnosticCode::UnknownEventSource, segments[1],
               "An event is raised on one target.",
               "'emit target.Event(...)' names a local name, a parameter or a field.",
               "Store the target in a local name first.");
        return NoSignature;
    }

    const uint32 last = segments[1];
    if (tree.Node(last).kind != SyntaxKind::Name) {
        return NoSignature;
    }

    const StringView name = TokenText(context.module, last);
    uint32 type = NoType;
    if (tree.Node(first).kind == SyntaxKind::This) {
        if (!binder_.EngineChainHas(context.owner, binder_.actor_)) {
            Report(context, DiagnosticCode::UnknownEventSource, first,
                   "'emit this.Event()' is only in an actor class.",
                   "It raises an actor extension event on the class's own actor.",
                   "Raise it on the actor, as in 'emit Owner.Event(...)'.");
            return NoSignature;
        }

        type = TypeOfSymbol(context.owner);
    } else if (tree.Node(first).kind == SyntaxKind::Name) {
        const uint32 symbol = c.NodeSymbol(context.module, first);
        if (symbol == NoSymbol) {
            return NoSignature;
        }

        const Symbol& entry = c.symbols_[symbol];
        switch (entry.kind) {
        case SymbolKind::Local:
        case SymbolKind::Parameter:
        case SymbolKind::EngineField:
            type = entry.type;
            break;
        case SymbolKind::Field:
            type = FieldType(symbol);
            break;
        case SymbolKind::ContextName:
            type = entry.name == "World" ? TypeOfSymbol(EngineSymbol("World")) : NoType;
            break;
        default:
            break;
        }

        if (type == NoType) {
            if (entry.kind != SymbolKind::Local && entry.kind != SymbolKind::Parameter &&
                entry.kind != SymbolKind::Field && entry.kind != SymbolKind::EngineField) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("'{}' is not an event target.", entry.name);
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::UnknownEventSource, first, message.View(),
                           "'emit target.Event(...)' names an actor or the World.",
                           "Use a local name, a parameter or a field that holds an actor.");
                }
            }

            return NoSignature;
        }

        // await 뒤의 객체 지역 이름(3.11절)과 `?` 타입은 발행할 수 없다.
        if ((entry.kind == SymbolKind::Local || entry.kind == SymbolKind::Parameter) &&
            context.module == staleModule_ && first < staleRead_.Size() && staleRead_[first] != 0 &&
            IsObjectType(type) && !IsOptional(type)) {
            ReportStaleRead(context, first);
            return NoSignature;
        }

        if (IsOptional(type)) {
            Report(context, DiagnosticCode::MayBeNull, first,
                   "The value may be null, so an event cannot be raised on it.",
                   "A value whose type has '?' is null when the object is gone or missing.",
                   "Check it first with 'if (let x = value) { emit x.Event(...); }'.");
            return NoSignature;
        }
    } else {
        return NoSignature;
    }

    const uint32 symbol = SymbolOf(type);
    uint32 target = NoSymbol;
    if (binder_.EngineChainHas(symbol, binder_.actor_)) {
        target = binder_.actor_;
    } else if (symbol != NoSymbol && symbol == EngineSymbol("World")) {
        target = symbol;
    }

    uint32 hidden = NoModule;
    const uint32 extension =
        target == NoSymbol ? NoSymbol : FindExtensionEvent(context.module, target, name, &hidden);
    if (extension != NoSymbol) {
        binder_.NoteUse(context.module, extension);
        return c.symbols_[extension].signature;
    }

    const uint32 member = FindMember(type, name);
    if (target == NoSymbol ||
        (member != NoSymbol && (c.symbols_[member].kind == SymbolKind::Event ||
                                c.symbols_[member].kind == SymbolKind::EngineEvent))) {
        Report(context, DiagnosticCode::EmitNotAllowed, last,
               "Only actor and World extension events are raised on a target.",
               "Member events are raised by the class that declares them, and engine events by "
               "the engine.",
               "Call a function of the target that raises the event.");
        return NoSignature;
    }

    if (hidden != NoModule) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat(
            "Extension event '{}' is declared in module '{}', which is not imported here.", name,
            c.Text(c.modules_[hidden].importPath));
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        fix.AppendFormat("Add 'import {};'.", c.Text(c.modules_[hidden].importPath));
        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            Report(context, DiagnosticCode::ExtensionEventNotImported, last, message.View(),
                   "Extension events are used only from modules that import their module "
                   "directly.",
                   fix.View());
        }

        return NoSignature;
    }

    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.AppendFormat("No extension event '{}' for this target.", name);
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::UnknownEvent, last, message.View(),
               "'emit target.Event(...)' raises an extension event declared as "
               "'event Actor.Event(...)' or 'event World.Event(...)'.",
               "Check the event name.");
    }

    return NoSignature;
}

void TypeChecker::CheckExtensionEvents() noexcept {
    const Compilation& c = compilation_;
    for (const uint32 event : extensionEvents_) {
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const Symbol& entry = c.symbols_[event];
        const SyntaxTree& tree = c.trees_[entry.module];
        CheckContext context;
        context.module = entry.module;
        if (entry.owner == NoSymbol) {
            const uint32 target = tree.Child(entry.node, 2);
            Report(context, DiagnosticCode::ExtensionEventTarget,
                   target == NoNode ? entry.nameNode : target,
                   "Extension events are declared on 'Actor' or 'World'.",
                   "Other objects declare events as members of their class.",
                   "Write 'event Actor.Name(...)' or 'event World.Name(...)'.");
            continue;
        }

        if (entry.name == ChangedEventName) {
            Report(context, DiagnosticCode::ChangedUnavailable, entry.nameNode,
                   "'Changed' is a reserved event name.",
                   "'X.Changed' always means a change of field X.", "Choose another event name.");
            continue;
        }

        const uint32 member = binder_.FindMemberInChain(entry.owner, entry.name);
        if (member != NoSymbol && c.symbols_[member].kind == SymbolKind::EngineEvent) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' is already an event of '{}'.", entry.name,
                                 c.symbols_[entry.owner].name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::DuplicateExtensionEvent, entry.nameNode,
                       message.View(), "Extension event names are unique for their target.",
                       "Choose another event name.");
            }

            continue;
        }

        // 같은 패키지의 앞선 선언, 또는 의존 패키지의 선언과 겹치면 이쪽이 S0814다.
        const uint32 package = c.modules_[entry.module].package;
        const PackageManifest& manifest = c.packages_[package].manifest;
        for (const uint32 other : extensionEvents_) {
            const Symbol& previous = c.symbols_[other];
            if (other == event || previous.owner != entry.owner || previous.name != entry.name) {
                continue;
            }

            const uint32 otherPackage = c.modules_[previous.module].package;
            bool conflict = otherPackage == package && DeclaredBefore(other, event);
            for (const ManifestName& dependency : manifest.Dependencies()) {
                conflict = conflict ||
                           (otherPackage != package &&
                            c.packages_[otherPackage].manifest.Name() == manifest.Text(dependency));
            }

            if (!conflict) {
                continue;
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Extension event '{}.{}' is already declared in module '{}'.",
                                 c.symbols_[entry.owner].name, entry.name,
                                 c.Text(c.modules_[previous.module].importPath));
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::DuplicateExtensionEvent, entry.nameNode,
                       message.View(),
                       "Extension event names are unique for their target in a package and its "
                       "dependencies.",
                       "Choose another event name.");
            }

            break;
        }
    }
}

void TypeChecker::CollectExtensionEvents() noexcept {
    const Compilation& c = compilation_;
    for (uint32 id = 0; id < c.symbols_.Size(); ++id) {
        const Symbol& entry = c.symbols_[id];
        if (entry.kind != SymbolKind::Event || entry.module >= c.modules_.Size() ||
            entry.node == NoNode ||
            c.trees_[entry.module].Node(entry.node).kind != SyntaxKind::ExtensionEvent) {
            continue;
        }

        if (auto pushed = extensionEvents_.TryPushBack(id); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }
}

} // namespace rocks::sparks::detail
