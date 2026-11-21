#include <Core/Algorithms.h>
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
    static constexpr uint32 NetWrite = 1u << 15; // `@net` 필드 쓰기(3.15절)
    static constexpr uint32 Rpc = 1u << 16;      // RPC 호출(3.15절)
    // 자리 표지: 권한 구역 안이다(그 자리의 `@net` 쓰기는 전하지 않는다 — 효과가 아니다).
    static constexpr uint32 InAuthority = 1u << 17;
    static constexpr uint32 Markers = ForeignReceiver | InAuthority;
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
    {EffectBits::EditWrite, "change serialized fields ('@edit', '@save', '@net')"},
    {EffectBits::ObjectWrite, "change other objects"},
    {EffectBits::Write, "change fields"},
    {EffectBits::EngineCall, "call engine functions that change state"},
    {EffectBits::OneShotCall, "play one-shot effects"},
    {EffectBits::Rpc, "call RPCs"},
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

bool TypeChecker::IsAttributeNumber(uint32 module, uint32 expression, bool integer,
                                    bool nonNegative) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    if (expression == NoNode) {
        return false;
    }

    SyntaxKind kind = tree.Node(expression).kind;
    if (kind == SyntaxKind::Unary && TokenText(module, expression) == "-") {
        if (nonNegative) {
            return false;
        }

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
    if (numeric.kind == NumericKind::None || (integer && numeric.kind == NumericKind::Float)) {
        return false;
    }

    const ConstantValue value = ConstOf(symbol);
    const bool negative =
        (value.kind == ConstantValue::Kind::Integer && value.negative && value.magnitude != 0) ||
        (value.kind == ConstantValue::Kind::Float && value.real < 0.0);
    return !nonNegative || !negative;
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

            if (!IsAttributeNumber(context.module, tree.Child(argument, 1), false, false)) {
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
            bad(arguments.Size() > 1 && shortName(arguments[0], Modes) ? arguments[1]
                                                                       : arguments[0],
                "'@net' takes nothing or 'OwnerOnly'.", "Write '@net(OwnerOnly)'.");
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
                bad(arguments.IsEmpty()                               ? attribute
                    : arguments.Size() > 1 && oneString(arguments[0]) ? arguments[1]
                                                                      : arguments[0],
                    message.View(), fix.View());
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
    case AttributeArguments::Rpc: {
        // 틀린 그 인자를 가리킨다(빠졌으면 attribute).
        uint32 at = NoNode;
        if (arguments.IsEmpty()) {
            at = attribute;
        } else if (!shortName(arguments[0], Span<const StringView>(RpcTargets))) {
            at = arguments[0];
        } else if (arguments.Size() >= 2 &&
                   !shortName(arguments[1], Span<const StringView>(RpcReliability))) {
            at = arguments[1];
        } else if (arguments.Size() > 2) {
            at = arguments[2];
        }

        if (at != NoNode) {
            bad(at,
                "'@rpc' takes 'Server', 'AllClients' or 'OwnerClient', then optionally "
                "'Reliable' or 'Unreliable'.",
                "Write '@rpc(Server, Reliable)'.");
        }

        break;
    }
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
                valid = IsAttributeNumber(context.module, value, false, true);
            } else {
                valid = IsAttributeNumber(context.module, value, true, false);
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
            if (entry.kind == SymbolKind::Function || entry.kind == SymbolKind::Method ||
                entry.kind == SymbolKind::InterfaceMethod) {
                return 0; // async fn 호출의 await는 정지 지점이 아니다(그 함수의 대기가 효과다)
            }

            // 인자가 모두 로드 요청인 `All`·`Race`는 준비 대기다(on Start에서 쓸 수 있다).
            if ((entry.flags & SymbolFlags::Variadic) != 0) {
                const uint32 list = tree.Child(operand, 1);
                bool ready = list != NoNode && tree.Node(list).childCount != 0;
                for (const uint32 argument :
                     list == NoNode ? Span<const uint32>() : tree.Children(list)) {
                    if (tree.Node(argument).kind != SyntaxKind::Argument) {
                        ready = false;
                        continue;
                    }

                    const uint32 value = tree.Child(argument, 1);
                    const uint32 type = value == NoNode ? NoType : c.ExpressionType(module, value);
                    ready = ready && type != NoType && IsLanguage(NonOptional(type), "LoadRequest");
                }

                return ready ? EffectBits::AwaitReady : EffectBits::AwaitOther;
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
                HasAttribute(field.module, field.node, "save") ||
                HasAttribute(field.module, field.node, "net")) {
                bits |= EffectBits::EditWrite;
            }

            if (HasAttribute(field.module, field.node, "net")) {
                bits |= EffectBits::NetWrite;
            }
        }
    }

    return bits;
}

void TypeChecker::CollectBodyEffects(uint32 bodyIndex) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[effectBodies_[bodyIndex].module];
    const uint32 module = effectBodies_[bodyIndex].module;
    const uint32 function = effectBodies_[bodyIndex].node;
    const bool handler = tree.Node(function).kind == SyntaxKind::Handler;
    const uint32 block = tree.Child(function, handler ? 4 : 5);
    const bool async = handler || (tree.Node(function).flags & SyntaxFlags::Async) != 0;
    CheckContext context;
    context.module = module;
    effectBodies_[bodyIndex].firstSite = static_cast<uint32>(effectSites_.Size());
    // 본문 전체가 권한 구역인지다: 서버 RPC, 권한 이벤트 handler(3.15절).
    const uint64 key = (uint64(module) << 32) | function;
    bool authorityBody = !handler && effectBodies_[bodyIndex].symbol != NoSymbol &&
                         RpcTarget(effectBodies_[bodyIndex].symbol) == "Server";
    for (const uint64 recorded : authorityHandlers_) {
        authorityBody = authorityBody || recorded == key;
    }

    // 자리마다 권한 구역 안인지 함께 둔다((구역 << 32) | 노드).
    Array<uint64> pending(allocator_);
    if (block != NoNode) {
        if (auto pushed = pending.TryPushBack((uint64(authorityBody ? 1 : 0) << 32) | block);
            !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }

    bool zone = false;
    auto site = [&](uint32 bits, uint32 node, uint32 callee) noexcept {
        if (zone) {
            bits = (bits & ~EffectBits::NetWrite) | EffectBits::InAuthority;
        }

        effectBodies_[bodyIndex].direct |= bits & ~EffectBits::Markers;
        if (auto pushed = effectSites_.TryPushBack(EffectSite{bodyIndex, node, bits, callee});
            !pushed) {
            binder_.error_ = pushed.Error();
        }
    };
    while (!pending.IsEmpty() && binder_.error_ == ErrorCode::None) {
        const uint64 item = pending.TryPopBack().Value(); // 비어 있지 않다
        const uint32 node = static_cast<uint32>(item);
        zone = (item >> 32) != 0;
        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::Await:
        case SyntaxKind::Try:
            if (entry.kind == SyntaxKind::Await || (entry.flags & SyntaxFlags::Await) != 0) {
                if (!async) {
                    // S0601: 그 await는 효과로 전하지 않는다(부르는 쪽의 S0602 연쇄 방지).
                    Report(context, DiagnosticCode::AwaitOutsideAsync, node,
                           "'await' is only in an 'async fn' or a handler.",
                           "A function that waits is declared 'async' so that its callers wait "
                           "for it too.",
                           "Declare the function as 'async fn'.");
                } else {
                    site(AwaitEffect(module, tree.Child(node, 0)), node, NoSymbol);
                }
            }

            break;
        case SyntaxKind::Emit:
            site(EffectBits::Emit, node, NoSymbol);
            break;
        case SyntaxKind::Assignment: {
            const uint32 bits = WriteEffect(module, tree.Child(node, 0));
            if (bits != 0) {
                site(bits, node, NoSymbol);
            }

            break;
        }
        case SyntaxKind::Call: {
            const uint32 callee = tree.Child(node, 0);
            const uint32 symbol = callee == NoNode ? NoSymbol : c.NodeSymbol(module, callee);
            if (symbol == NoSymbol) {
                break;
            }

            const Symbol& target = c.symbols_[symbol];
            uint32 object =
                tree.Node(callee).kind == SyntaxKind::Member ? tree.Child(callee, 0) : NoNode;
            while (object != NoNode && tree.Node(object).kind == SyntaxKind::Parenthesized) {
                object = tree.Child(object, 0);
            }

            const bool foreign = object != NoNode && tree.Node(object).kind != SyntaxKind::This;
            const uint32 prefix = object == NoNode ? NoSymbol : c.NodeSymbol(module, object);
            if (prefix != NoSymbol && c.symbols_[prefix].kind == SymbolKind::RpcPrefix) {
                site(EffectBits::Rpc, node,
                     NoSymbol); // RPC는 보내기만 한다(그 함수의 효과는 오지 않는다)
            } else if (target.kind == SymbolKind::Function || target.kind == SymbolKind::Method ||
                       target.kind == SymbolKind::InterfaceMethod) {
                site(foreign ? EffectBits::ForeignReceiver : 0, node, symbol);
            } else if (target.kind == SymbolKind::EngineFunction) {
                const SchemaFunction& schema =
                    c.schemas_[target.index].schema.Functions()[target.item];
                uint32 bits = foreign ? EffectBits::ForeignReceiver : 0;
                bits |= (schema.effects & SchemaEffects::Spawn) != 0 ? EffectBits::Spawn : 0;
                bits |= (schema.effects & SchemaEffects::Acquire) != 0 ? EffectBits::Acquire : 0;
                bits |= (schema.effects & (SchemaEffects::Io | SchemaEffects::WallClock)) != 0
                            ? EffectBits::Io
                            : 0;
                bits |= schema.oneShot ? EffectBits::OneShotCall
                        : schema.pure  ? 0
                                       : EffectBits::EngineCall;
                site(bits, node, symbol); // 엔진 overridable 함수는 재정의의 효과도 온다
            } else if (target.kind == SymbolKind::LanguageFunction) {
                if (target.name == "Load" || target.name == "LoadAll") {
                    site(EffectBits::Load, node, NoSymbol);
                }

                // 값 타입 경로를 바꾸는 변경 메서드는 그 경로의 쓰기다.
                if (IsMutatingMember(symbol) && tree.Node(callee).kind == SyntaxKind::Member) {
                    const uint32 bits = WriteEffect(module, tree.Child(callee, 0));
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
            const uint32 symbol = c.NodeSymbol(module, node);
            const SymbolKind kind =
                symbol == NoSymbol ? SymbolKind::BuiltinType : c.symbols_[symbol].kind;
            const uint32 type = c.ExpressionType(module, node);
            // class 필드(자기 객체)·엔진 필드·문맥 이름·객체 값(struct 필드는 값이다).
            const bool classField = kind == SymbolKind::Field &&
                                    c.symbols_[symbol].owner != NoSymbol &&
                                    c.symbols_[c.symbols_[symbol].owner].kind == SymbolKind::Class;
            if (classField || kind == SymbolKind::EngineField || kind == SymbolKind::ContextName ||
                (type != NoType && IsObjectType(NonOptional(type)))) {
                site(EffectBits::ObjectRef, node, NoSymbol);
            }

            break;
        }
        default:
            break;
        }

        // 권한 구역(3.15절): 권한 시험 `if`의 참 분기, 부정 권한 시험 `if`의 else 분기, 그리고
        // 블록에서 "부정 권한 시험이면 반환"하는 `if` 뒤의 문장(조기 반환 관용구).
        const bool isIf = entry.kind == SyntaxKind::If;
        const uint32 thenNode = isIf ? tree.Child(node, 1) : NoNode;
        const uint32 elseNode = isIf && entry.childCount > 2 ? tree.Child(node, 2) : NoNode;
        const bool authority = isIf && IsAuthorityTest(module, tree.Child(node, 0));
        const bool negated = isIf && IsNegatedAuthorityTest(module, tree.Child(node, 0));
        bool guarded = false;
        for (const uint32 child : tree.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            const bool childZone = zone || guarded || (authority && child == thenNode) ||
                                   (negated && child == elseNode);
            if (entry.kind == SyntaxKind::Block && tree.Node(child).kind == SyntaxKind::If &&
                IsNegatedAuthorityTest(module, tree.Child(child, 0)) &&
                AlwaysReturns(module, tree.Child(child, 1))) {
                guarded = true;
            }

            if (auto pushed = pending.TryPushBack((uint64(childZone ? 1 : 0) << 32) | child);
                !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }
        }
    }

    effectBodies_[bodyIndex].siteCount =
        static_cast<uint32>(effectSites_.Size()) - effectBodies_[bodyIndex].firstSite;
}

void TypeChecker::AddDispatch(uint32 body) noexcept {
    // 이 본문(class 메서드)이 대신하는 기반 함수(여러 단계의 재정의)와 interface 메서드다.
    const Compilation& c = compilation_;
    const EffectBody& entry = effectBodies_[body];
    if (entry.symbol == NoSymbol || entry.owner == NoSymbol ||
        c.symbols_[entry.owner].kind != SymbolKind::Class) {
        return;
    }

    const StringView name = c.symbols_[entry.symbol].name;
    auto add = [&](uint32 base) noexcept {
        const uint64 pair = (uint64(base) << 32) | body;
        if (auto pushed = dispatchPairs_.TryPushBack(pair); !pushed) {
            binder_.error_ = pushed.Error();
        }
    };
    if ((c.symbols_[entry.symbol].flags & SymbolFlags::Override) != 0) {
        uint32 owner = entry.owner;
        for (uint32 step = 0; owner != NoSymbol && step < c.symbols_.Size(); ++step) {
            const uint32 base = c.symbols_[owner].base;
            const uint32 member =
                base == NoSymbol ? NoSymbol : binder_.FindMemberInChain(base, name);
            if (member == NoSymbol) {
                break;
            }

            const SymbolKind kind = c.symbols_[member].kind;
            if (kind != SymbolKind::Method && kind != SymbolKind::EngineFunction) {
                break;
            }

            add(member);
            if (kind != SymbolKind::Method) {
                break; // 엔진 함수 위로는 Sparks 재정의가 없다
            }

            owner = c.symbols_[member].owner;
        }
    }

    // 구현한 interface(자기·기반 class의 기반 목록)의 같은 이름 메서드.
    uint32 step = 0;
    for (uint32 type = entry.owner; type != NoSymbol && step < c.symbols_.Size();
         type = c.symbols_[type].base, ++step) {
        const Symbol& classEntry = c.symbols_[type];
        if (classEntry.kind != SymbolKind::Class) {
            break;
        }

        for (uint32 index = 0; index < classEntry.baseCount; ++index) {
            const uint32 base = c.symbolBases_[classEntry.firstBase + index];
            const Symbol& baseEntry = c.symbols_[base];
            const bool engineInterface =
                baseEntry.kind == SymbolKind::EngineType &&
                c.schemas_[baseEntry.index].schema.Types()[baseEntry.item].kind ==
                    SchemaTypeKind::Interface;
            if (baseEntry.kind != SymbolKind::Interface && !engineInterface) {
                continue;
            }

            const uint32 member = binder_.FindMemberInChain(base, name);
            if (member != NoSymbol && (c.symbols_[member].kind == SymbolKind::InterfaceMethod ||
                                       c.symbols_[member].kind == SymbolKind::EngineFunction)) {
                add(member);
            }
        }
    }
}

void TypeChecker::AddInheritedImplementations() noexcept {
    // class가 구현하는 interface 메서드의 실제 구현이 기반 class의 것이면(상속한 함수로 구현) 그
    // 본문을 interface 메서드의 디스패치에 더한다.
    const Compilation& c = compilation_;
    for (uint32 id = 0; id < c.symbols_.Size() && binder_.error_ == ErrorCode::None; ++id) {
        const Symbol& classEntry = c.symbols_[id];
        if (classEntry.kind != SymbolKind::Class) {
            continue;
        }

        for (uint32 index = 0; index < classEntry.baseCount; ++index) {
            const uint32 base = c.symbolBases_[classEntry.firstBase + index];
            if (c.symbols_[base].kind != SymbolKind::Interface) {
                continue;
            }

            const Symbol& face = c.symbols_[base];
            const SyntaxTree& tree = c.trees_[face.module];
            const uint32 methods = tree.Child(face.node, 3);
            if (methods == NoNode || tree.Node(methods).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 method : tree.Children(methods)) {
                const uint32 required = tree.Node(method).kind == SyntaxKind::InterfaceMethod
                                            ? c.NodeSymbol(face.module, tree.Child(method, 2))
                                            : NoSymbol;
                if (required == NoSymbol) {
                    continue;
                }

                const uint32 implementation =
                    binder_.FindMemberInChain(id, c.symbols_[required].name);
                if (implementation == NoSymbol || c.symbols_[implementation].owner == id) {
                    continue; // 없거나 자기 함수(AddDispatch가 더했다)
                }

                const uint32 body = effectOfSymbol_[implementation];
                if (body != NoLocalIndex) {
                    if (auto pushed = dispatchPairs_.TryPushBack((uint64(required) << 32) | body);
                        !pushed) {
                        binder_.error_ = pushed.Error();
                        return;
                    }
                }
            }
        }
    }
}

template <typename Visit>
void TypeChecker::ForEachCallTarget(uint32 callee, Visit&& visit) const noexcept {
    if (callee == NoSymbol) {
        return;
    }

    const uint32 own = effectOfSymbol_[callee];
    if (own != NoLocalIndex) {
        visit(own);
    }

    for (uint32 index = dispatchFirst_[callee]; index < dispatchFirst_[callee + 1]; ++index) {
        visit(dispatchBodies_[index]);
    }
}

uint32 TypeChecker::SiteEffects(const EffectSite& site) const noexcept {
    uint32 bits = site.bits & ~EffectBits::Markers;
    ForEachCallTarget(site.callee,
                      [&](uint32 body) noexcept { bits |= effectBodies_[body].total; });
    // 다른 객체의 함수가 바꾸는 필드는 이 본문에서 보면 객체 쓰기다.
    if ((site.bits & EffectBits::ForeignReceiver) != 0 && (bits & EffectBits::Write) != 0) {
        bits |= EffectBits::ObjectWrite;
    }

    // 권한 구역 안의 자리는 `@net` 쓰기를 전하지 않는다(3.15절).
    if ((site.bits & EffectBits::InAuthority) != 0) {
        bits &= ~EffectBits::NetWrite;
    }

    return bits;
}

void TypeChecker::AnalyzeEffects() noexcept {
    const Compilation& c = compilation_;
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    if (auto resized = effectOfSymbol_.TryResize(symbolCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32& index : effectOfSymbol_) {
        index = NoLocalIndex;
    }

    // 본문을 모은다(최상위·class·struct 함수, handler).
    auto addBody = [&](uint32 module, uint32 node, uint32 symbol, uint32 owner) noexcept {
        if (auto pushed = effectBodies_.TryPushBack(EffectBody{module, node, symbol, owner});
            !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        if (symbol != NoSymbol) {
            effectOfSymbol_[symbol] = static_cast<uint32>(effectBodies_.Size() - 1);
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

    const uint32 bodyCount = static_cast<uint32>(effectBodies_.Size());
    for (uint32 index = 0; index < bodyCount && binder_.error_ == ErrorCode::None; ++index) {
        CollectBodyEffects(index);
        AddDispatch(index);
    }

    AddInheritedImplementations();

    // 디스패치 표(기반·interface 함수 심볼 → 그것을 대신하는 본문들)다.
    algo::Sort(dispatchPairs_.View());
    if (auto resized = dispatchFirst_.TryResize(usize(symbolCount) + 1); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    if (auto resized = dispatchBodies_.TryResize(dispatchPairs_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32& first : dispatchFirst_) {
        first = 0;
    }

    for (usize index = 0; index < dispatchPairs_.Size(); ++index) {
        ++dispatchFirst_[(dispatchPairs_[index] >> 32) + 1];
        dispatchBodies_[index] = static_cast<uint32>(dispatchPairs_[index]);
    }

    for (uint32 symbol = 0; symbol < symbolCount; ++symbol) {
        dispatchFirst_[symbol + 1] += dispatchFirst_[symbol];
    }

    // 역호출(부른 본문 → 부른 쪽 본문) 표다.
    Array<uint64> reversePairs(allocator_);
    for (const EffectSite& site : effectSites_) {
        ForEachCallTarget(site.callee, [&](uint32 target) noexcept {
            if (auto pushed = reversePairs.TryPushBack((uint64(target) << 32) | site.body);
                !pushed) {
                binder_.error_ = pushed.Error();
            }
        });
    }

    if (binder_.error_ != ErrorCode::None) {
        return;
    }

    algo::Sort(reversePairs.View());
    Array<uint32> callerFirst(allocator_);
    if (auto resized = callerFirst.TryResize(usize(bodyCount) + 1); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32& first : callerFirst) {
        first = 0;
    }

    for (const uint64 pair : reversePairs) {
        ++callerFirst[(pair >> 32) + 1];
    }

    for (uint32 body = 0; body < bodyCount; ++body) {
        callerFirst[body + 1] += callerFirst[body];
    }

    // worklist 고정점: 본문 효과 = 직접 효과 ∪ 부른 본문(재정의·interface 구현 포함)의 효과.
    Array<uint32> work(allocator_);
    Array<uint8> queued(allocator_);
    if (auto resized = queued.TryResize(bodyCount); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 body = 0; body < bodyCount; ++body) {
        effectBodies_[body].total = effectBodies_[body].direct;
        queued[body] = 1;
        if (auto pushed = work.TryPushBack(body); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }
    }

    while (!work.IsEmpty()) {
        const uint32 body = work.TryPopBack().Value(); // 비어 있지 않다
        queued[body] = 0;
        EffectBody& entry = effectBodies_[body];
        uint32 total = entry.direct;
        for (uint32 index = entry.firstSite; index < entry.firstSite + entry.siteCount; ++index) {
            total |= SiteEffects(effectSites_[index]);
        }

        if (total == entry.total) {
            continue;
        }

        entry.total = total;
        for (uint32 index = callerFirst[body]; index < callerFirst[body + 1]; ++index) {
            const uint32 caller = static_cast<uint32>(reversePairs[index]);
            if (queued[caller] == 0) {
                queued[caller] = 1;
                if (auto pushed = work.TryPushBack(caller); !pushed) {
                    binder_.error_ = pushed.Error();
                    return;
                }
            }
        }
    }

    for (uint32 index = 0; index < bodyCount && binder_.error_ == ErrorCode::None; ++index) {
        CheckBodyContext(index);
    }
}

StringView TypeChecker::ContextAttribute(uint32 body) const noexcept {
    // 자기 attribute, 없으면 재정의한 기반 함수 사슬의 것(재정의는 기반 문맥을 따른다).
    const Compilation& c = compilation_;
    uint32 module = effectBodies_[body].module;
    uint32 node = effectBodies_[body].node;
    uint32 symbol = effectBodies_[body].symbol;
    for (uint32 step = 0; step < c.symbols_.Size(); ++step) {
        const SyntaxTree& tree = c.trees_[module];
        const uint32 attributes = tree.Child(node, 1);
        if (attributes != NoNode && tree.Node(attributes).kind == SyntaxKind::List) {
            for (const uint32 attribute : tree.Children(attributes)) {
                const StringView text = tree.Node(attribute).kind == SyntaxKind::Attribute
                                            ? TokenText(module, attribute)
                                            : StringView();
                if (text == "pure" || text == "worker" || text == "deterministic") {
                    return text;
                }
            }
        }

        if (symbol == NoSymbol || (c.symbols_[symbol].flags & SymbolFlags::Override) == 0) {
            return StringView();
        }

        const uint32 owner = c.symbols_[symbol].owner;
        const uint32 base = owner == NoSymbol ? NoSymbol : c.symbols_[owner].base;
        const uint32 member =
            base == NoSymbol ? NoSymbol : binder_.FindMemberInChain(base, c.symbols_[symbol].name);
        if (member == NoSymbol || c.symbols_[member].kind != SymbolKind::Method) {
            return StringView();
        }

        symbol = member;
        module = c.symbols_[member].module;
        node = c.symbols_[member].node;
    }

    return StringView();
}

bool TypeChecker::StatementWaits(uint32 module, uint32 node) noexcept {
    // 정지하는 `await`가 있는지다(async fn 호출의 await는 그 함수가 대기할 때만).
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
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
            const uint32 operand = tree.Child(current, 0);
            if (AwaitEffect(module, operand) != 0) {
                return true;
            }

            uint32 call = operand;
            while (call != NoNode && tree.Node(call).kind == SyntaxKind::Parenthesized) {
                call = tree.Child(call, 0);
            }

            const uint32 callee = call != NoNode && tree.Node(call).kind == SyntaxKind::Call
                                      ? c.NodeSymbol(module, tree.Child(call, 0))
                                      : NoSymbol;
            bool waits = false;
            ForEachCallTarget(callee, [&](uint32 body) noexcept {
                waits = waits || (effectBodies_[body].total & EffectBits::AnyAwait) != 0;
            });
            if (waits) {
                return true;
            }
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

void TypeChecker::CheckBodyContext(uint32 bodyIndex) noexcept {
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
                forbidden = Await | EffectBits::Spawn | EffectBits::Io | EffectBits::EditWrite |
                            EffectBits::Rpc;
            }

            where.AppendFormat("'on {}()'", name);
        } else if (name == "Changed") {
            forbidden = Await;
            where.Append("A 'Changed' handler");
        }
    } else {
        const StringView context = ContextAttribute(bodyIndex);
        if (context == "pure") {
            forbidden = EffectBits::Write | EffectBits::ObjectWrite | EffectBits::EditWrite |
                        EffectBits::Emit | Await | EffectBits::Spawn | EffectBits::Acquire |
                        EffectBits::Load | EffectBits::EngineCall | EffectBits::OneShotCall |
                        EffectBits::Rpc;
        } else if (context == "worker") {
            forbidden = EffectBits::ObjectRef | EffectBits::Acquire | EffectBits::Load | Await |
                        EffectBits::Emit | EffectBits::Rpc;
        } else if (context == "deterministic") {
            // RPC는 네트워크 송신이라 재생하면 다시 나간다.
            forbidden =
                EffectBits::Io | EffectBits::AwaitOther | EffectBits::AwaitReady | EffectBits::Rpc;
        }

        // 최상위 fn은 순수 계산 함수다(3.1절): 대기·발행·엔진 변경 API·객체 쓰기를 쓸 수 없다.
        // `@test` 함수는 시험 World에서 도는 진입점이라 제외한다(§13).
        const bool test = HasAttribute(body.module, body.node, "test");
        const bool topLevel = body.owner == NoSymbol && !test;
        if (topLevel && (entry.flags & SyntaxFlags::Async) != 0) {
            Report(CheckContext{body.module}, DiagnosticCode::AwaitOutsideAsync,
                   tree.Child(body.node, 2), "A top-level function cannot be 'async'.",
                   "Top-level functions are plain calculations; they never wait.",
                   "Remove 'async', or move the function into a class.");
        }

        if (topLevel) {
            forbidden |= Await | EffectBits::Emit | EffectBits::Spawn | EffectBits::Acquire |
                         EffectBits::Load | EffectBits::EngineCall | EffectBits::OneShotCall |
                         EffectBits::ObjectWrite;
        }

        if (!context.IsEmpty()) {
            where.AppendFormat("An '@{}' function", context);
        } else if (topLevel) {
            where.Append("A top-level function");
        }
    }

    CheckContext context;
    context.module = body.module;
    // 같은 위치의 진단은 하나다(S0602·S1003 공통 — 알린 위치의 목록, 알림은 드물어 선형 검색).
    Array<uint32> reported(allocator_);
    auto firstAt = [&](uint32 node) noexcept {
        const uint32 at = tree.Node(node).span.begin;
        for (const uint32 previous : reported) {
            if (previous == at) {
                return false;
            }
        }

        if (auto pushed = reported.TryPushBack(at); !pushed) {
            binder_.error_ = pushed.Error();
            return false;
        }

        return true;
    };
    if (forbidden != 0 && binder_.Succeeded(where)) {
        for (uint32 index = body.firstSite; index < body.firstSite + body.siteCount; ++index) {
            const EffectSite& site = effectSites_[index];
            const uint32 bits = SiteEffects(site) & forbidden;
            if (bits == 0 || !firstAt(site.node)) {
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

    // 권한 구역 밖의 `@net` 쓰기(3.15절 S1003): 진입 본문(handler·hook·`@rpc` 함수·엔진
    // overridable 재정의·`@cheat` 함수)에서 본다.
    const bool entryBody =
        handler || (body.symbol != NoSymbol &&
                    (!RpcTarget(body.symbol).IsEmpty() || OverridesEngineFunction(body.symbol) ||
                     HasAttribute(body.module, body.node, "cheat")));
    if (entryBody) {
        const bool actor = binder_.EngineChainHas(body.owner, binder_.actor_);
        for (uint32 index = body.firstSite; index < body.firstSite + body.siteCount; ++index) {
            const EffectSite& site = effectSites_[index];
            if ((SiteEffects(site) & EffectBits::NetWrite) == 0 || !firstAt(site.node)) {
                continue;
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            if (site.callee != NoSymbol) {
                message.AppendFormat("'{}' writes a replicated field outside an authority zone.",
                                     c.symbols_[site.callee].name);
            } else {
                message.Append("A replicated field is written outside an authority zone.");
            }

            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::NetWriteOutsideAuthority, site.node, message.View(),
                       "'@net' fields change only where this object has authority, so every "
                       "machine sees the same value.",
                       actor ? StringView("Wrap it in 'if (HasAuthority) { ... }'.")
                             : StringView("Wrap it in 'if (Owner.HasAuthority) { ... }'."));
            }
        }
    }

    // `while`: 대기할 수 있는 본문에서만, 회차마다 `await`(3.13절 S0603).
    const bool canWait = (handler || (entry.flags & SyntaxFlags::Async) != 0) &&
                         (forbidden & EffectBits::AwaitOther) == 0;
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
        return StatementWaits(module, node) ? WaitStateWaited : states;
    }
}

bool TypeChecker::IsAuthorityTest(uint32 module, uint32 expression) const noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    while (expression != NoNode && tree.Node(expression).kind == SyntaxKind::Parenthesized) {
        expression = tree.Child(expression, 0);
    }

    if (expression == NoNode) {
        return false;
    }

    const SyntaxNode& entry = tree.Node(expression);
    if (entry.kind == SyntaxKind::Binary && TokenText(module, expression) == "&&") {
        return IsAuthorityTest(module, tree.Child(expression, 0)) ||
               IsAuthorityTest(module, tree.Child(expression, 1));
    }

    // 이름이 아니라 Actor의 `HasAuthority`·Component의 `Owner` 심볼인지 본다(다른 타입의 같은
    // 이름 필드는 권한 시험이 아니다).
    auto engineMember = [&](uint32 node, uint32 owner, StringView name) noexcept {
        const uint32 symbol = c.NodeSymbol(module, node);
        return symbol != NoSymbol && owner != NoSymbol &&
               c.symbols_[symbol].kind == SymbolKind::EngineField &&
               c.symbols_[symbol].owner == owner && c.symbols_[symbol].name == name;
    };
    auto hasAuthority = [&](uint32 node) noexcept {
        return engineMember(node, binder_.actor_, "HasAuthority");
    };
    if (entry.kind == SyntaxKind::Identifier) {
        return hasAuthority(expression); // Actor 파생의 `HasAuthority`
    }

    if (entry.kind != SyntaxKind::Member || !hasAuthority(expression)) {
        return false;
    }

    // `Owner.HasAuthority`(component).
    uint32 object = tree.Child(expression, 0);
    while (object != NoNode && tree.Node(object).kind == SyntaxKind::Parenthesized) {
        object = tree.Child(object, 0);
    }

    return object != NoNode && tree.Node(object).kind == SyntaxKind::Identifier &&
           engineMember(object, binder_.component_, "Owner");
}

bool TypeChecker::IsNegatedAuthorityTest(uint32 module, uint32 expression) const noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    while (expression != NoNode && tree.Node(expression).kind == SyntaxKind::Parenthesized) {
        expression = tree.Child(expression, 0);
    }

    if (expression == NoNode) {
        return false;
    }

    const SyntaxNode& entry = tree.Node(expression);
    if (entry.kind == SyntaxKind::Binary && TokenText(module, expression) == "||") {
        return IsNegatedAuthorityTest(module, tree.Child(expression, 0)) ||
               IsNegatedAuthorityTest(module, tree.Child(expression, 1));
    }

    return entry.kind == SyntaxKind::Unary && TokenText(module, expression) == "!" &&
           IsAuthorityTest(module, tree.Child(expression, 0));
}

bool TypeChecker::AlwaysReturns(uint32 module, uint32 statement) const noexcept {
    // `return` 하나이거나 마지막 문장이 `return`인 블록이다(조기 반환 관용구의 모양만 본다).
    const SyntaxTree& tree = compilation_.trees_[module];
    if (statement == NoNode) {
        return false;
    }

    if (tree.Node(statement).kind == SyntaxKind::Return) {
        return true;
    }

    const SyntaxNode& entry = tree.Node(statement);
    return entry.kind == SyntaxKind::Block && entry.childCount > 0 &&
           AlwaysReturns(module, tree.Child(statement, entry.childCount - 1));
}

} // namespace rocks::sparks::detail
