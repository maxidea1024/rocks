#include <Core/Text/EditDistance.h>

#include "CompilationChecker.h"
#include "TypeChecker.h"

// 실행 문맥·효과·attribute 규칙이다(Docs/Design/SparksLanguage.ko.md 3.13절): attribute 이름·자리·
// 중복·인자(S0620–S0623), `async` 밖의 `await`(S0601), 문맥이 금지하는 효과(S0602 — 호출 그래프의
// 전이 폐포), `while`의 대기(S0603). Rules 단계(LC3)에서만 돈다.

namespace rocks::sparks::detail {

namespace {

// 효과 비트(3.13절 표)다.
struct EffectBits {
    static constexpr uint32 AwaitOther = 1u << 0;   // 대기(준비 대기·NextFixedTick 밖)
    static constexpr uint32 AwaitFixed = 1u << 1;   // `await NextFixedTick()`
    static constexpr uint32 AwaitReady = 1u << 2;   // 준비 대기
    static constexpr uint32 Spawn = 1u << 3;        // 생성
    static constexpr uint32 Acquire = 1u << 4;      // 자원 획득
    static constexpr uint32 Load = 1u << 5;         // 로드
    static constexpr uint32 Emit = 1u << 6;         // 발행
    static constexpr uint32 Write = 1u << 7;        // 필드 쓰기
    static constexpr uint32 ObjectWrite = 1u << 8;  // 객체를 지난 쓰기
    static constexpr uint32 EngineCall = 1u << 9;   // pure·oneShot이 아닌 엔진 함수
    static constexpr uint32 OneShotCall = 1u << 10; // 일회성 효과 엔진 함수
    static constexpr uint32 Io = 1u << 11;          // 입출력·실제 시간
    static constexpr uint32 ObjectRef = 1u << 12;   // 객체 값 사용
    static constexpr uint32 EditWrite = 1u << 13;   // `@edit`·`@save` 필드 쓰기
    static constexpr uint32 AnyAwait = AwaitOther | AwaitFixed | AwaitReady;
    // 호출 자리 표지: 수신자가 다른 객체다(그 호출의 쓰기는 객체 쓰기 — 효과가 아니다).
    static constexpr uint32 ForeignReceiver = 1u << 14;
};

// 진단 문장의 효과 설명(비트 순서 — 표 순서로 처음 걸린 효과를 알린다)이다.
struct EffectText {
    uint32 bit;
    StringView verb; // "'on Tick()' cannot {verb}."
};

constexpr EffectText EffectTexts[] = {
    {EffectBits::AwaitOther, "wait"},
    {EffectBits::AwaitFixed, "wait"},
    {EffectBits::AwaitReady, "wait"},
    {EffectBits::Spawn, "spawn actors"},
    {EffectBits::Acquire, "acquire held resources"},
    {EffectBits::Load, "load assets"},
    {EffectBits::Emit, "raise events"},
    {EffectBits::EditWrite, "change '@edit' or '@save' fields"},
    {EffectBits::ObjectWrite, "change other objects"},
    {EffectBits::Write, "change fields"},
    {EffectBits::EngineCall, "call engine functions that change state"},
    {EffectBits::OneShotCall, "play one-shot effects"},
    {EffectBits::Io, "use input, output or the wall clock"},
    {EffectBits::ObjectRef, "use engine objects or script objects"},
};

// attribute를 둘 수 있는 자리다.
struct AttributePlace {
    static constexpr uint32 ClassField = 1u << 0;
    static constexpr uint32 StructField = 1u << 1;
    static constexpr uint32 Event = 1u << 2; // 멤버·확장 이벤트
    static constexpr uint32 EnumCase = 1u << 3;
    static constexpr uint32 TypeDeclaration = 1u << 4; // class·struct·enum
    static constexpr uint32 Struct = 1u << 5;
    static constexpr uint32 Handler = 1u << 6;
    static constexpr uint32 ClassFunction = 1u << 7;
    static constexpr uint32 StructFunction = 1u << 8;
    static constexpr uint32 TopFunction = 1u << 9;
    static constexpr uint32 TickHook = 1u << 10;
    static constexpr uint32 AnyFunction = ClassFunction | StructFunction | TopFunction;
};

// attribute 인자 모양이다.
enum class AttributeArguments : uint8 {
    None,
    EditRange, // Min:·Max:
    NetMode,   // OwnerOnly
    OneString, // "..."
    AssetLiteral,
    Rpc,
    Tick,
};

struct AttributeRule {
    StringView name;
    uint32 places;
    AttributeArguments arguments;
    bool varOnly; // `var` 필드만
    bool context; // 실행 문맥 attribute(함수 하나에 하나)
};

constexpr AttributeRule AttributeRules[] = {
    {"edit", AttributePlace::ClassField | AttributePlace::StructField,
     AttributeArguments::EditRange, false, false},
    {"save", AttributePlace::ClassField, AttributeArguments::None, true, false},
    {"net", AttributePlace::ClassField, AttributeArguments::NetMode, true, false},
    {"authorityOnly", AttributePlace::Event, AttributeArguments::None, false, false},
    {"formerly",
     AttributePlace::ClassField | AttributePlace::StructField | AttributePlace::EnumCase |
         AttributePlace::TypeDeclaration,
     AttributeArguments::OneString, false, false},
    {"tableRow", AttributePlace::Struct, AttributeArguments::AssetLiteral, false, false},
    {"skipWhileBusy", AttributePlace::Handler | AttributePlace::AnyFunction,
     AttributeArguments::None, false, false},
    {"restart", AttributePlace::Handler | AttributePlace::AnyFunction, AttributeArguments::None,
     false, false},
    {"rpc", AttributePlace::ClassFunction, AttributeArguments::Rpc, false, false},
    {"pure", AttributePlace::AnyFunction, AttributeArguments::None, false, true},
    {"worker", AttributePlace::AnyFunction, AttributeArguments::None, false, true},
    {"deterministic", AttributePlace::AnyFunction, AttributeArguments::None, false, true},
    {"editorButton", AttributePlace::ClassFunction, AttributeArguments::None, false, true},
    {"cheat", AttributePlace::ClassFunction, AttributeArguments::None, false, true},
    {"test", AttributePlace::TopFunction, AttributeArguments::None, false, true},
    {"tick", AttributePlace::TickHook, AttributeArguments::Tick, false, false},
    {"cvar", AttributePlace::ClassField, AttributeArguments::OneString, true, false},
};

constexpr StringView TickPhaseNames[] = {"PrePhysics", "PostPhysics", "LateUpdate"};
constexpr StringView RpcTargets[] = {"Server", "AllClients", "OwnerClient"};
constexpr StringView RpcReliability[] = {"Reliable", "Unreliable"};

// attribute 이름 제안의 편집 거리 상한이다.
constexpr usize MaxAttributeSuggestionDistance = 2;

} // namespace

// ---- attribute ----

bool TypeChecker::IsAttributeNumber(uint32 module, uint32 expression, bool integer) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    if (expression == NoNode) {
        return false;
    }

    SyntaxKind kind = tree.Node(expression).kind;
    if (kind == SyntaxKind::Unary && TokenText(module, expression) == "-") {
        expression = tree.Child(expression, 0);
        kind = expression == NoNode ? SyntaxKind::Error : tree.Node(expression).kind;
    }

    if (kind == SyntaxKind::IntegerLiteral) {
        return true;
    }

    if (kind == SyntaxKind::FloatLiteral) {
        return !integer;
    }

    // const 이름(attribute 인자는 결합하지 않으므로 모듈의 최상위·import 이름과 별칭 멤버로
    // 찾는다).
    uint32 symbol = NoSymbol;
    if (kind == SyntaxKind::Identifier) {
        const StringView name = TokenText(module, expression);
        symbol = binder_.FindName(SpaceKey(NameSpaceKind::ModuleTop, module), name);
        if (symbol == NoSymbol) {
            symbol = binder_.FindName(SpaceKey(NameSpaceKind::ModuleImport, module), name);
        }
    } else if (kind == SyntaxKind::Member) {
        const uint32 object = tree.Child(expression, 0);
        if (object != NoNode && tree.Node(object).kind == SyntaxKind::Identifier) {
            const uint32 alias = binder_.FindName(SpaceKey(NameSpaceKind::ModuleImport, module),
                                                  TokenText(module, object));
            if (alias != NoSymbol && c.symbols_[alias].kind == SymbolKind::Alias) {
                symbol =
                    binder_.FindName(SpaceKey(NameSpaceKind::ModuleTop, c.symbols_[alias].index),
                                     TokenText(module, expression));
            }
        }
    }

    if (symbol == NoSymbol || c.symbols_[symbol].kind != SymbolKind::Const) {
        return false;
    }

    binder_.NoteUse(module, symbol);
    const NumericInfo numeric = Numeric(c.symbols_[symbol].type);
    return numeric.kind != NumericKind::None && (!integer || numeric.kind != NumericKind::Float);
}

void TypeChecker::CheckAttributeArguments(const CheckContext& context, uint32 declaration,
                                          uint32 attribute, StringView name, uint8 shape) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 list = tree.Child(attribute, 0);
    const Span<const uint32> arguments = list == NoNode || tree.Node(list).kind != SyntaxKind::List
                                             ? Span<const uint32>()
                                             : tree.Children(list);
    auto bad = [&](uint32 at, StringView message, StringView fix) noexcept {
        Report(context, DiagnosticCode::AttributeArgument, at, message,
               "Attributes take only the arguments listed for them.", fix);
    };
    auto argumentName = [&](uint32 argument) noexcept {
        const uint32 nameNode = tree.Child(argument, 0);
        return nameNode == NoNode ? StringView() : TokenText(context.module, nameNode);
    };
    auto shortName = [&](uint32 argument, Span<const StringView> allowed) noexcept {
        const uint32 value = tree.Child(argument, 1);
        if (tree.Child(argument, 0) != NoNode || value == NoNode ||
            tree.Node(value).kind != SyntaxKind::Identifier) {
            return false;
        }

        const StringView text = TokenText(context.module, value);
        for (const StringView candidate : allowed) {
            if (candidate == text) {
                return true;
            }
        }

        return false;
    };
    auto oneString = [&](uint32 argument) noexcept {
        const uint32 value = tree.Child(argument, 1);
        if (tree.Child(argument, 0) != NoNode || value == NoNode) {
            return false;
        }

        const SyntaxKind kind = tree.Node(value).kind;
        if (kind == SyntaxKind::VerbatimString) {
            return true;
        }

        if (kind != SyntaxKind::String) {
            return false;
        }

        for (const uint32 part : tree.Children(value)) {
            if (tree.Node(part).kind == SyntaxKind::Interpolation) {
                return false;
            }
        }

        return true;
    };

    switch (static_cast<AttributeArguments>(shape)) {
    case AttributeArguments::None:
        if (!arguments.IsEmpty()) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'@{}' takes no arguments.", name);
            if (binder_.Succeeded(message)) {
                bad(arguments[0], message.View(), "Remove the arguments.");
            }
        }

        break;
    case AttributeArguments::EditRange: {
        bool seenMin = false;
        bool seenMax = false;
        for (const uint32 argument : arguments) {
            const StringView key = argumentName(argument);
            const bool isMin = key == "Min";
            if ((key != "Min" && key != "Max") || (isMin ? seenMin : seenMax)) {
                bad(argument, "'@edit' takes 'Min:' and 'Max:' once each.",
                    "Write '@edit(Min: 0, Max: 10)'.");
                return;
            }

            (isMin ? seenMin : seenMax) = true;
            const uint32 field = c.NodeSymbol(context.module, tree.Child(declaration, 2));
            const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
            if (type != NoType && Numeric(type).kind == NumericKind::None) {
                bad(argument, "'Min' and 'Max' are only for number fields.", "Remove the range.");
                return;
            }

            if (!IsAttributeNumber(context.module, tree.Child(argument, 1), false)) {
                bad(tree.Child(argument, 1), "A range bound is a number or a number constant.",
                    "Write a number, as in 'Min: 0'.");
                return;
            }
        }

        break;
    }
    case AttributeArguments::NetMode: {
        constexpr StringView Modes[] = {"OwnerOnly"};
        if (arguments.Size() > 1 || (arguments.Size() == 1 && !shortName(arguments[0], Modes))) {
            bad(arguments[0], "'@net' takes nothing or 'OwnerOnly'.", "Write '@net(OwnerOnly)'.");
        }

        break;
    }
    case AttributeArguments::OneString:
        if (arguments.Size() != 1 || !oneString(arguments[0])) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'@{}' takes one string without interpolation.", name);
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            fix.AppendFormat("Write '@{}(\"Name\")'.", name);
            if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
                bad(arguments.IsEmpty() ? attribute : arguments[0], message.View(), fix.View());
            }
        }

        break;
    case AttributeArguments::AssetLiteral: {
        bool valid = arguments.Size() == 1 && tree.Child(arguments[0], 0) == NoNode;
        const uint32 value = valid ? tree.Child(arguments[0], 1) : NoNode;
        valid = valid && value != NoNode && tree.Node(value).kind == SyntaxKind::Call;
        const uint32 callee = valid ? tree.Child(value, 0) : NoNode;
        valid = valid && callee != NoNode && tree.Node(callee).kind == SyntaxKind::Identifier &&
                TokenText(context.module, callee) == "Asset";
        if (!valid) {
            bad(arguments.IsEmpty() ? attribute : arguments[0],
                "'@tableRow' takes the data table as 'Asset(\"...\")'.",
                "Write '@tableRow(Asset(\"/Game/Data/Table\"))'.");
        }

        break;
    }
    case AttributeArguments::Rpc:
        if (arguments.IsEmpty() || arguments.Size() > 2 ||
            !shortName(arguments[0], Span<const StringView>(RpcTargets)) ||
            (arguments.Size() == 2 &&
             !shortName(arguments[1], Span<const StringView>(RpcReliability)))) {
            bad(arguments.IsEmpty() ? attribute : arguments[0],
                "'@rpc' takes 'Server', 'AllClients' or 'OwnerClient', then optionally "
                "'Reliable' or 'Unreliable'.",
                "Write '@rpc(Server, Reliable)'.");
        }

        break;
    case AttributeArguments::Tick: {
        bool seen[3] = {false, false, false};
        if (arguments.IsEmpty()) {
            bad(attribute, "'@tick' takes 'Phase:', 'Interval:' or 'Priority:'.",
                "Write '@tick(Phase: PostPhysics)'.");
            return;
        }

        for (const uint32 argument : arguments) {
            const StringView key = argumentName(argument);
            const uint32 value = tree.Child(argument, 1);
            const usize slot = key == "Phase"      ? 0
                               : key == "Interval" ? 1
                               : key == "Priority" ? 2
                                                   : 3;
            if (slot == 3 || seen[slot]) {
                bad(argument, "'@tick' takes 'Phase:', 'Interval:' and 'Priority:' once each.",
                    "Write '@tick(Phase: PostPhysics, Interval: 0.5)'.");
                return;
            }

            seen[slot] = true;
            bool valid = false;
            if (slot == 0) {
                valid = value != NoNode && tree.Node(value).kind == SyntaxKind::Identifier;
                for (usize index = 0; valid && index < 1; ++index) {
                    const StringView text = TokenText(context.module, value);
                    valid = false;
                    for (const StringView phase : TickPhaseNames) {
                        valid = valid || phase == text;
                    }
                }
            } else if (slot == 1) {
                valid = value != NoNode && tree.Node(value).kind != SyntaxKind::Unary &&
                        IsAttributeNumber(context.module, value, false);
            } else {
                valid = IsAttributeNumber(context.module, value, true);
            }

            if (!valid) {
                bad(value == NoNode ? argument : value,
                    slot == 0   ? StringView("'Phase' is 'PrePhysics', 'PostPhysics' or "
                                               "'LateUpdate'.")
                    : slot == 1 ? StringView("'Interval' is a number of seconds, 0 or more.")
                                : StringView("'Priority' is an integer."),
                    "Write '@tick(Phase: PostPhysics, Interval: 0.5, Priority: 1)'.");
                return;
            }
        }

        break;
    }
    }
}

void TypeChecker::CheckAttributeList(const CheckContext& context, uint32 declaration, uint32 place,
                                     bool isVar, bool noParameters, bool worldServiceVar) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 attributes = tree.Child(declaration, 1);
    if (attributes == NoNode || tree.Node(attributes).kind != SyntaxKind::List) {
        return;
    }

    const Span<const uint32> list = tree.Children(attributes);
    bool contextSeen = false;
    for (usize index = 0; index < list.Size(); ++index) {
        const uint32 attribute = list[index];
        if (tree.Node(attribute).kind != SyntaxKind::Attribute) {
            continue;
        }

        const StringView name = TokenText(context.module, attribute);
        const AttributeRule* rule = nullptr;
        for (const AttributeRule& candidate : AttributeRules) {
            if (candidate.name == name) {
                rule = &candidate;
            }
        }

        if (rule == nullptr) {
            StringView best;
            usize bestDistance = MaxAttributeSuggestionDistance + 1;
            for (const AttributeRule& candidate : AttributeRules) {
                const usize distance =
                    EditDistance(name, candidate.name, MaxAttributeSuggestionDistance,
                                 EditDistanceCase::AsciiInsensitive);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate.name;
                }
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Unknown attribute '@{}'.", name);
            StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
            if (best.IsEmpty()) {
                fix.Append("Remove the attribute.");
            } else {
                fix.AppendFormat("Write '@{}'.", best);
            }

            if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
                Report(context, DiagnosticCode::UnknownAttribute, attribute, message.View(),
                       "Attributes are a fixed set defined by the language.", fix.View());
            }

            continue;
        }

        bool duplicate = false;
        for (usize previous = 0; previous < index; ++previous) {
            duplicate = duplicate || (tree.Node(list[previous]).kind == SyntaxKind::Attribute &&
                                      TokenText(context.module, list[previous]) == name);
        }

        if (duplicate) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'@{}' is written twice.", name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::DuplicateAttribute, attribute, message.View(),
                       "An attribute is written once on a declaration.", "Remove one of them.");
            }

            continue;
        }

        const bool allowed =
            (rule->places & place) != 0 && (!rule->varOnly || isVar) &&
            !(rule->context && contextSeen) &&
            ((name != "editorButton" && name != "cheat" && name != "test") || noParameters) &&
            (name != "cvar" || worldServiceVar) &&
            (name != "rpc" || (tree.Node(declaration).flags & SyntaxFlags::Async) == 0);
        if (!allowed) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            if (rule->context && contextSeen) {
                message.AppendFormat("'@{}' cannot be combined with another execution context.",
                                     name);
            } else {
                message.AppendFormat("'@{}' cannot be used here.", name);
            }

            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::AttributeNotAllowed, attribute, message.View(),
                       "Each attribute applies to particular declarations (see the attribute "
                       "table).",
                       "Move the attribute or remove it.");
            }

            continue;
        }

        contextSeen = contextSeen || rule->context;
        if (name == "skipWhileBusy" || name == "restart") {
            continue; // 3.12절(S0817)이 자리를 본다
        }

        CheckAttributeArguments(context, declaration, attribute, name,
                                static_cast<uint8>(rule->arguments));
    }
}

void TypeChecker::CheckAttributes(uint32 module) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;
    const uint32 worldService = binder_.worldService_;
    auto hasParameters = [&](uint32 function) noexcept {
        const uint32 parameters = tree.Child(function, 3);
        return parameters != NoNode && tree.Node(parameters).childCount != 0;
    };
    for (const uint32 node : tree.Children(tree.Child(tree.Root(), 1))) {
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::Function:
            CheckAttributeList(context, node, AttributePlace::TopFunction, false,
                               !hasParameters(node), false);
            break;
        case SyntaxKind::ExtensionEvent:
            CheckAttributeList(context, node, AttributePlace::Event, false, false, false);
            break;
        case SyntaxKind::Const:
        case SyntaxKind::Interface:
            CheckAttributeList(context, node, 0, false, false, false);
            break;
        case SyntaxKind::Enum: {
            CheckAttributeList(context, node, AttributePlace::TypeDeclaration, false, false, false);
            const uint32 cases = tree.Child(node, 3);
            if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
                for (const uint32 item : tree.Children(cases)) {
                    if (tree.Node(item).kind == SyntaxKind::EnumCase) {
                        CheckAttributeList(context, item, AttributePlace::EnumCase, false, false,
                                           false);
                    }
                }
            }

            break;
        }
        case SyntaxKind::Class:
        case SyntaxKind::Struct: {
            const bool isClass = entry.kind == SyntaxKind::Class;
            CheckAttributeList(context, node,
                               AttributePlace::TypeDeclaration |
                                   (isClass ? 0u : AttributePlace::Struct),
                               false, false, false);
            const uint32 owner = c.NodeSymbol(module, tree.Child(node, 2));
            const bool service = isClass && binder_.EngineChainHas(owner, worldService);
            const uint32 members = tree.Child(node, isClass ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                break;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& memberEntry = tree.Node(member);
                if (memberEntry.kind == SyntaxKind::Field) {
                    const bool isVar = (memberEntry.flags & SyntaxFlags::Let) == 0;
                    const uint32 field = c.NodeSymbol(module, tree.Child(member, 2));
                    const uint32 type = field == NoSymbol ? NoType : c.symbols_[field].type;
                    const bool scalar =
                        type != NoType && (type == boolType_ || type == stringType_ ||
                                           Numeric(type).kind != NumericKind::None);
                    CheckAttributeList(context, member,
                                       isClass ? AttributePlace::ClassField
                                               : AttributePlace::StructField,
                                       isVar, false, service && isVar && scalar);
                } else if (memberEntry.kind == SyntaxKind::Function) {
                    CheckAttributeList(context, member,
                                       isClass ? AttributePlace::ClassFunction
                                               : AttributePlace::StructFunction,
                                       false, !hasParameters(member), false);
                } else if (memberEntry.kind == SyntaxKind::Event) {
                    CheckAttributeList(context, member, AttributePlace::Event, false, false, false);
                } else if (memberEntry.kind == SyntaxKind::Handler) {
                    const uint32 path = tree.Child(member, 2);
                    const bool tickHook = path != NoNode && tree.Node(path).childCount == 1 &&
                                          tree.Node(tree.Child(path, 0)).kind == SyntaxKind::Name &&
                                          (TokenText(module, tree.Child(path, 0)) == "Tick" ||
                                           TokenText(module, tree.Child(path, 0)) == "FixedTick" ||
                                           TokenText(module, tree.Child(path, 0)) == "LateTick");
                    CheckAttributeList(context, member,
                                       AttributePlace::Handler |
                                           (tickHook ? AttributePlace::TickHook : 0u),
                                       false, false, false);
                }
            }

            break;
        }
        default:
            break;
        }
    }
}

// ---- 효과 ----

uint32 TypeChecker::AwaitEffect(uint32 module, uint32 operand) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    while (operand != NoNode && tree.Node(operand).kind == SyntaxKind::Parenthesized) {
        operand = tree.Child(operand, 0);
    }

    if (operand == NoNode) {
        return EffectBits::AwaitOther;
    }

    if (tree.Node(operand).kind == SyntaxKind::Call) {
        const uint32 callee = tree.Child(operand, 0);
        const uint32 symbol = callee == NoNode ? NoSymbol : c.NodeSymbol(module, callee);
        if (symbol != NoSymbol) {
            const Symbol& entry = c.symbols_[symbol];
            if (entry.kind == SymbolKind::Function || entry.kind == SymbolKind::Method) {
                return 0; // async fn 호출의 await는 정지 지점이 아니다(그 함수의 대기가 효과다)
            }

            if ((entry.kind == SymbolKind::LanguageFunction ||
                 entry.kind == SymbolKind::PreludeValue) &&
                entry.name == "NextFixedTick") {
                return EffectBits::AwaitFixed;
            }

            if (entry.kind == SymbolKind::LanguageFunction &&
                (entry.name == "Load" || entry.name == "LoadAll")) {
                return EffectBits::AwaitReady;
            }

            if (entry.kind == SymbolKind::EngineFunction &&
                (c.schemas_[entry.index].schema.Functions()[entry.item].effects &
                 SchemaEffects::Readiness) != 0) {
                return EffectBits::AwaitReady;
            }
        }
    }

    const uint32 type = c.ExpressionType(module, operand);
    if (type != NoType && IsLanguage(NonOptional(type), "LoadRequest")) {
        return EffectBits::AwaitReady;
    }

    return EffectBits::AwaitOther;
}

uint32 TypeChecker::WriteEffect(uint32 module, uint32 target) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    uint32 root = target;
    bool crosses = false;
    while (root != NoNode) {
        const SyntaxKind kind = tree.Node(root).kind;
        if (kind == SyntaxKind::Member || kind == SyntaxKind::Index) {
            const uint32 object = tree.Child(root, 0);
            const uint32 type = c.ExpressionType(module, object);
            crosses = crosses || (type != NoType && IsReference(NonOptional(type)));
            root = object;
        } else if (kind == SyntaxKind::Parenthesized) {
            root = tree.Child(root, 0);
        } else {
            break;
        }
    }

    if (root == NoNode || tree.Node(root).kind != SyntaxKind::Identifier) {
        return crosses ? EffectBits::Write | EffectBits::ObjectWrite : 0;
    }

    const uint32 symbol = c.NodeSymbol(module, root);
    const SymbolKind kind = symbol == NoSymbol ? SymbolKind::BuiltinType : c.symbols_[symbol].kind;
    uint32 bits = crosses ? EffectBits::Write | EffectBits::ObjectWrite : 0;
    if (kind == SymbolKind::Field || kind == SymbolKind::EngineField) {
        bits |= EffectBits::Write;
        if (kind == SymbolKind::Field && !crosses) {
            const Symbol& field = c.symbols_[symbol];
            if (HasAttribute(field.module, field.node, "edit") ||
                HasAttribute(field.module, field.node, "save")) {
                bits |= EffectBits::EditWrite;
            }
        }
    }

    return bits;
}

void TypeChecker::CollectBodyEffects(uint32 bodyIndex) noexcept {
    const Compilation& c = compilation_;
    EffectBody& body = effectBodies_[bodyIndex];
    const SyntaxTree& tree = c.trees_[body.module];
    const bool handler = tree.Node(body.node).kind == SyntaxKind::Handler;
    const uint32 block = tree.Child(body.node, handler ? 4 : 5);
    const bool async = handler || (tree.Node(body.node).flags & SyntaxFlags::Async) != 0;
    CheckContext context;
    context.module = body.module;
    Array<uint32> pending(allocator_);
    if (block == NoNode) {
        return;
    }

    if (auto pushed = pending.TryPushBack(block); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    auto site = [&](uint32 bits, uint32 node, uint32 callee) noexcept {
        body.direct |= bits & ~EffectBits::ForeignReceiver;
        if (auto pushed = effectSites_.TryPushBack(EffectSite{bodyIndex, node, bits, callee});
            !pushed) {
            binder_.error_ = pushed.Error();
        }
    };
    while (!pending.IsEmpty() && binder_.error_ == ErrorCode::None) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::Await:
        case SyntaxKind::Try:
            if (entry.kind == SyntaxKind::Await || (entry.flags & SyntaxFlags::Await) != 0) {
                if (!async) {
                    Report(context, DiagnosticCode::AwaitOutsideAsync, node,
                           "'await' is only in an 'async fn' or a handler.",
                           "A function that waits is declared 'async' so that its callers wait "
                           "for it too.",
                           "Declare the function as 'async fn'.");
                }

                site(AwaitEffect(body.module, tree.Child(node, 0)), node, NoSymbol);
            }

            break;
        case SyntaxKind::Emit:
            site(EffectBits::Emit, node, NoSymbol);
            break;
        case SyntaxKind::Assignment: {
            const uint32 bits = WriteEffect(body.module, tree.Child(node, 0));
            if (bits != 0) {
                site(bits, node, NoSymbol);
            }

            break;
        }
        case SyntaxKind::Call: {
            const uint32 callee = tree.Child(node, 0);
            const uint32 symbol = callee == NoNode ? NoSymbol : c.NodeSymbol(body.module, callee);
            if (symbol == NoSymbol) {
                break;
            }

            const Symbol& target = c.symbols_[symbol];
            if (target.kind == SymbolKind::Function || target.kind == SymbolKind::Method) {
                uint32 object =
                    tree.Node(callee).kind == SyntaxKind::Member ? tree.Child(callee, 0) : NoNode;
                while (object != NoNode && tree.Node(object).kind == SyntaxKind::Parenthesized) {
                    object = tree.Child(object, 0);
                }

                const bool foreign = object != NoNode && tree.Node(object).kind != SyntaxKind::This;
                site(foreign ? EffectBits::ForeignReceiver : 0, node, symbol);
            } else if (target.kind == SymbolKind::EngineFunction) {
                const SchemaFunction& function =
                    c.schemas_[target.index].schema.Functions()[target.item];
                uint32 bits = 0;
                bits |= (function.effects & SchemaEffects::Spawn) != 0 ? EffectBits::Spawn : 0;
                bits |= (function.effects & SchemaEffects::Acquire) != 0 ? EffectBits::Acquire : 0;
                bits |= (function.effects & (SchemaEffects::Io | SchemaEffects::WallClock)) != 0
                            ? EffectBits::Io
                            : 0;
                bits |= function.oneShot ? EffectBits::OneShotCall
                        : function.pure  ? 0
                                         : EffectBits::EngineCall;
                site(bits, node, symbol);
            } else if (target.kind == SymbolKind::LanguageFunction) {
                if (target.name == "Load" || target.name == "LoadAll") {
                    site(EffectBits::Load, node, NoSymbol);
                }

                // 값 타입 경로를 바꾸는 변경 메서드는 그 경로의 쓰기다.
                if (IsMutatingMember(symbol) && tree.Node(callee).kind == SyntaxKind::Member) {
                    const uint32 bits = WriteEffect(body.module, tree.Child(callee, 0));
                    if (bits != 0) {
                        site(bits, node, NoSymbol);
                    }
                }
            }

            break;
        }
        case SyntaxKind::This:
            site(EffectBits::ObjectRef, node, NoSymbol);
            break;
        case SyntaxKind::Identifier: {
            const uint32 symbol = c.NodeSymbol(body.module, node);
            const SymbolKind kind =
                symbol == NoSymbol ? SymbolKind::BuiltinType : c.symbols_[symbol].kind;
            const uint32 type = c.ExpressionType(body.module, node);
            if (kind == SymbolKind::Field || kind == SymbolKind::EngineField ||
                kind == SymbolKind::ContextName ||
                (type != NoType && IsObjectType(NonOptional(type)))) {
                site(EffectBits::ObjectRef, node, NoSymbol);
            }

            break;
        }
        default:
            break;
        }

        for (const uint32 child : tree.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }
}

void TypeChecker::AnalyzeEffects() noexcept {
    const Compilation& c = compilation_;
    // 본문을 모은다(최상위·class·struct 함수, handler).
    if (auto resized = effectOfSymbol_.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32& index : effectOfSymbol_) {
        index = NoLocalIndex;
    }

    auto addBody = [&](uint32 module, uint32 node, uint32 symbol, uint32 owner) noexcept {
        if (symbol != NoSymbol) {
            effectOfSymbol_[symbol] = static_cast<uint32>(effectBodies_.Size());
        }

        if (auto pushed = effectBodies_.TryPushBack(EffectBody{module, node, symbol, owner, 0, 0});
            !pushed) {
            binder_.error_ = pushed.Error();
        }
    };
    for (uint32 module = 0; module < c.modules_.Size(); ++module) {
        if (c.nodeBase_[module] == NoNode) {
            continue;
        }

        const SyntaxTree& tree = c.trees_[module];
        for (const uint32 node : tree.Children(tree.Child(tree.Root(), 1))) {
            const SyntaxNode& entry = tree.Node(node);
            if (entry.kind == SyntaxKind::Function) {
                addBody(module, node, c.NodeSymbol(module, tree.Child(node, 2)), NoSymbol);
                continue;
            }

            if (entry.kind != SyntaxKind::Class && entry.kind != SyntaxKind::Struct) {
                continue;
            }

            const uint32 owner = c.NodeSymbol(module, tree.Child(node, 2));
            const uint32 members = tree.Child(node, entry.kind == SyntaxKind::Class ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxKind kind = tree.Node(member).kind;
                if (kind == SyntaxKind::Function) {
                    addBody(module, member, c.NodeSymbol(module, tree.Child(member, 2)), owner);
                } else if (kind == SyntaxKind::Handler) {
                    addBody(module, member, NoSymbol, owner);
                }
            }
        }
    }

    for (uint32 index = 0; index < effectBodies_.Size() && binder_.error_ == ErrorCode::None;
         ++index) {
        CollectBodyEffects(index);
    }

    // 재정의: 기반 함수(사용자·엔진)를 부르면 재정의의 효과도 더한다.
    Array<uint32> overrideBase(allocator_);
    if (auto resized = overrideBase.TryResize(effectBodies_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 index = 0; index < effectBodies_.Size(); ++index) {
        overrideBase[index] = NoSymbol;
        const EffectBody& body = effectBodies_[index];
        if (body.symbol == NoSymbol || body.owner == NoSymbol ||
            (c.symbols_[body.symbol].flags & SymbolFlags::Override) == 0) {
            continue;
        }

        const uint32 base = c.symbols_[body.owner].base;
        overrideBase[index] = base == NoSymbol
                                  ? NoSymbol
                                  : binder_.FindMemberInChain(base, c.symbols_[body.symbol].name);
    }

    // 고정점: 본문 효과 = 직접 효과 ∪ 부른 함수의 효과 ∪ (부른 함수가 기반이면) 재정의의 효과.
    for (EffectBody& body : effectBodies_) {
        body.total = body.direct;
    }

    bool changed = true;
    while (changed && binder_.error_ == ErrorCode::None) {
        changed = false;
        for (const EffectSite& site : effectSites_) {
            if (site.callee == NoSymbol) {
                continue;
            }

            const uint32 bits =
                SiteEffects(site, overrideBase.View()) & ~EffectBits::ForeignReceiver;
            EffectBody& caller = effectBodies_[site.body];
            const uint32 merged = caller.total | bits;
            if (merged != caller.total) {
                caller.total = merged;
                changed = true;
            }
        }
    }

    for (uint32 index = 0; index < effectBodies_.Size() && binder_.error_ == ErrorCode::None;
         ++index) {
        CheckBodyContext(index, overrideBase.View());
    }
}

uint32 TypeChecker::SiteEffects(const EffectSite& site,
                                Span<const uint32> overrideBase) const noexcept {
    uint32 bits = site.bits;
    if (site.callee == NoSymbol) {
        return bits;
    }

    const uint32 callee = effectOfSymbol_[site.callee];
    if (callee != NoLocalIndex) {
        bits |= effectBodies_[callee].total;
    }

    for (uint32 index = 0; index < effectBodies_.Size(); ++index) {
        if (overrideBase[index] == site.callee) {
            bits |= effectBodies_[index].total;
        }
    }

    // 다른 객체의 함수가 바꾸는 필드는 이 본문에서 보면 객체 쓰기다.
    if ((site.bits & EffectBits::ForeignReceiver) != 0 && (bits & EffectBits::Write) != 0) {
        bits |= EffectBits::ObjectWrite;
    }

    return bits;
}

void TypeChecker::CheckBodyContext(uint32 bodyIndex, Span<const uint32> overrideBase) noexcept {
    const Compilation& c = compilation_;
    const EffectBody& body = effectBodies_[bodyIndex];
    const SyntaxTree& tree = c.trees_[body.module];
    const SyntaxNode& entry = tree.Node(body.node);
    const bool handler = entry.kind == SyntaxKind::Handler;
    uint32 forbidden = 0;
    StringBuilder<CompilationMessageInlineBytes> where(allocator_);
    constexpr uint32 Await = EffectBits::AnyAwait;
    if (handler) {
        const uint32 path = tree.Child(body.node, 2);
        const uint32 count = path == NoNode ? 0 : tree.Node(path).childCount;
        const uint32 last = count == 0 ? NoNode : tree.Child(path, count - 1);
        const StringView name = last == NoNode || tree.Node(last).kind != SyntaxKind::Name
                                    ? StringView()
                                    : TokenText(body.module, last);
        if (count == 1) {
            if (name == "Awake") {
                forbidden = Await | EffectBits::Spawn | EffectBits::Acquire | EffectBits::Load;
            } else if (name == "Enable") {
                forbidden = Await;
            } else if (name == "Disable") {
                forbidden = Await | EffectBits::Acquire | EffectBits::Emit;
            } else if (name == "Start") {
                forbidden = EffectBits::AwaitOther | EffectBits::AwaitFixed;
            } else if (name == "Tick" || name == "FixedTick" || name == "LateTick") {
                forbidden = Await | EffectBits::Spawn | EffectBits::Acquire | EffectBits::Load;
            } else if (name == "Destroy") {
                forbidden = Await | EffectBits::Spawn | EffectBits::Acquire | EffectBits::Emit |
                            EffectBits::ObjectWrite | EffectBits::EngineCall;
            } else if (name == "Validate") {
                forbidden = Await | EffectBits::Spawn | EffectBits::Io | EffectBits::EditWrite;
            }

            where.AppendFormat("'on {}()'", name);
        } else if (name == "Changed") {
            forbidden = Await;
            where.Append("A 'Changed' handler");
        }
    } else {
        const uint32 attributes = tree.Child(body.node, 1);
        StringView context;
        if (attributes != NoNode && tree.Node(attributes).kind == SyntaxKind::List) {
            for (const uint32 attribute : tree.Children(attributes)) {
                const StringView text = tree.Node(attribute).kind == SyntaxKind::Attribute
                                            ? TokenText(body.module, attribute)
                                            : StringView();
                if (text == "pure" || text == "worker" || text == "deterministic") {
                    context = text;
                    break;
                }
            }
        }

        if (context == "pure") {
            forbidden = EffectBits::Write | EffectBits::ObjectWrite | EffectBits::EditWrite |
                        EffectBits::Emit | Await | EffectBits::Spawn | EffectBits::Acquire |
                        EffectBits::Load | EffectBits::EngineCall | EffectBits::OneShotCall;
        } else if (context == "worker") {
            forbidden = EffectBits::ObjectRef | EffectBits::Acquire | EffectBits::Load | Await |
                        EffectBits::Emit;
        } else if (context == "deterministic") {
            forbidden = EffectBits::Io | EffectBits::AwaitOther | EffectBits::AwaitReady;
        }

        if (!context.IsEmpty()) {
            where.AppendFormat("An '@{}' function", context);
        }
    }

    if (forbidden != 0 && binder_.Succeeded(where)) {
        CheckContext context;
        context.module = body.module;
        for (const EffectSite& site : effectSites_) {
            if (site.body != bodyIndex) {
                continue;
            }

            const uint32 bits = SiteEffects(site, overrideBase) & forbidden;
            if (bits == 0) {
                continue;
            }

            StringView verb;
            for (const EffectText& text : EffectTexts) {
                if (verb.IsEmpty() && (bits & text.bit) != 0) {
                    verb = text.verb;
                }
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            if (site.callee != NoSymbol && (site.bits & bits) == 0) {
                message.AppendFormat("{} cannot {}, and '{}' does.", where.View(), verb,
                                     c.symbols_[site.callee].name);
            } else {
                message.AppendFormat("{} cannot {}.", where.View(), verb);
            }

            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ContextEffect, site.node, message.View(),
                       "Each hook and execution context limits what its code may do (the "
                       "execution context table).",
                       "Move this code to a hook or function that allows it, such as 'on Start()' "
                       "or an event handler.");
            }
        }
    }

    // `while`: 대기할 수 있는 본문에서만, 회차마다 `await`(3.13절 S0603).
    const bool canWait = (handler && (forbidden & EffectBits::AwaitOther) == 0) ||
                         (!handler && (entry.flags & SyntaxFlags::Async) != 0 &&
                          (forbidden & EffectBits::AwaitOther) == 0);
    CheckContext context;
    context.module = body.module;
    Array<uint32> pending(allocator_);
    const uint32 block = tree.Child(body.node, handler ? 4 : 5);
    if (block == NoNode) {
        return;
    }

    if (auto pushed = pending.TryPushBack(block); !pushed) {
        binder_.error_ = pushed.Error();
        return;
    }

    while (!pending.IsEmpty()) {
        const uint32 node = pending.TryPopBack().Value(); // 비어 있지 않다
        if (tree.Node(node).kind == SyntaxKind::While) {
            if (!canWait) {
                Report(context, DiagnosticCode::WhileWithoutWait, node,
                       "'while' is only where the code can wait.",
                       "A 'while' loop repeats over frames, so it is used in an 'async fn' or a "
                       "handler that can wait.",
                       "Use 'for (i in 0..<count)' to repeat a fixed number of times.");
            } else {
                bool continuesWithoutWait = false;
                const uint8 state = WaitStates(body.module, tree.Child(node, 1), WaitStateNotWaited,
                                               continuesWithoutWait);
                if (continuesWithoutWait || (state & WaitStateNotWaited) != 0) {
                    Report(context, DiagnosticCode::WhileWithoutWait, node,
                           "Every pass of this 'while' must wait.",
                           "A loop that can repeat without 'await' would run within one frame "
                           "forever.",
                           "Add 'await NextTick();' to the loop.");
                }
            }
        }

        for (const uint32 child : tree.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            if (auto pushed = pending.TryPushBack(child); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }
}

uint8 TypeChecker::WaitStates(uint32 module, uint32 node, uint8 states,
                              bool& continuesWithoutWait) noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    if (node == NoNode || states == 0) {
        return states;
    }

    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::Block:
        for (const uint32 statement : tree.Children(node)) {
            states = WaitStates(module, statement, states, continuesWithoutWait);
        }

        return states;
    case SyntaxKind::If: {
        const uint32 otherwise = tree.Child(node, 2);
        return static_cast<uint8>(
            WaitStates(module, tree.Child(node, 1), states, continuesWithoutWait) |
            (otherwise == NoNode ? states
                                 : WaitStates(module, otherwise, states, continuesWithoutWait)));
    }
    case SyntaxKind::Switch: {
        const uint32 fallback = tree.Child(node, 2);
        const uint64 key = (uint64(module) << 32) | node;
        bool exhaustive = false;
        for (const uint64 recorded : exhaustiveSwitches_) {
            exhaustive = exhaustive || recorded == key;
        }

        uint8 out = fallback == NoNode && !exhaustive ? states : 0;
        const uint32 cases = tree.Child(node, 1);
        if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
            for (const uint32 item : tree.Children(cases)) {
                if (tree.Node(item).kind == SyntaxKind::SwitchCase) {
                    out |= WaitStates(module, tree.Child(item, 1), states, continuesWithoutWait);
                }
            }
        }

        if (fallback != NoNode && tree.Node(fallback).kind == SyntaxKind::SwitchDefault) {
            out |= WaitStates(module, tree.Child(fallback, 0), states, continuesWithoutWait);
        }

        return out;
    }
    case SyntaxKind::For:
    case SyntaxKind::While: {
        // 안쪽 반복의 본문은 0번 돌 수 있다. 안쪽의 `continue`·`break`는 안쪽 반복의 것이다.
        bool inner = false;
        const uint32 body = tree.Child(node, entry.kind == SyntaxKind::For ? 4 : 1);
        return static_cast<uint8>(states | WaitStates(module, body, states, inner));
    }
    case SyntaxKind::Break:
    case SyntaxKind::Return:
        return 0; // 반복을 나간다
    case SyntaxKind::Continue:
        continuesWithoutWait = continuesWithoutWait || (states & WaitStateNotWaited) != 0;
        return 0;
    default:
        return ContainsAwait(module, node) ? WaitStateWaited : states;
    }
}

} // namespace rocks::sparks::detail
