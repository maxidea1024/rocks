#include <Core/Text/EditDistance.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "TypeChecker.h"

// 멤버 접근(S0326·S0412·S0414·S0425)과 호출(S0402–S0404·S0411·S0422), 생성·숫자 변환(S0502·S0508),
// 검증 리터럴(S0417·S0424)이다. 규칙: Docs/Design/SparksLanguage 3.10절, 부록 B 시그니처.

namespace rocks::sparks::detail {

namespace {

// 찾지 못한 멤버에 비슷한 이름을 제시하는 편집 거리 상한이다(이름 해석과 같은 값).
constexpr usize MaxMemberSuggestionDistance = 2;

} // namespace

namespace {

bool IsStaticFunction(const Symbol& symbol) noexcept {
    return (symbol.kind == SymbolKind::EngineFunction ||
            symbol.kind == SymbolKind::LanguageFunction) &&
           (symbol.flags & SymbolFlags::Static) != 0;
}

} // namespace

uint32 TypeChecker::FindMember(uint32 type, StringView name) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol) {
        return NoSymbol;
    }

    const SymbolKind kind = compilation_.symbols_[symbol].kind;
    if (kind == SymbolKind::BuiltinType || kind == SymbolKind::PreludeType) {
        return binder_.FindName(SpaceKey(NameSpaceKind::Members, symbol), name);
    }

    return binder_.FindMemberInChain(symbol, name);
}

bool TypeChecker::HasIncompleteMembers(uint32 type) const noexcept {
    uint32 symbol = SymbolOf(type);
    for (uint32 step = 0; symbol != NoSymbol && step < compilation_.symbols_.Size(); ++step) {
        if ((compilation_.symbols_[symbol].flags & SymbolFlags::Incomplete) != 0) {
            return true;
        }

        symbol = compilation_.symbols_[symbol].base;
    }

    return false;
}

ExpressionValue TypeChecker::CheckMember(const CheckContext& context, uint32 node) noexcept {
    // 재귀 경로(`a.b.c ...`)의 frame에는 받는 값만 둔다(CheckBinary 참고).
    const ExpressionValue receiver =
        Check(context, compilation_.trees_[context.module].Child(node, 0), NoType);
    return MemberOf(context, node, receiver);
}

ExpressionValue TypeChecker::MemberOf(const CheckContext& context, uint32 node,
                                      const ExpressionValue& receiver) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = MemberOfValue(context, node, receiver);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::MemberOfValue(const CheckContext& context, uint32 node,
                                           const ExpressionValue& receiver) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 objectNode = tree.Child(node, 0);
    const bool chained = (tree.Node(node).flags & SyntaxFlags::NullConditional) != 0;
    const StringView name = TokenText(context.module, node);
    ExpressionValue result;
    if (receiver.kind == K::Error || name.IsEmpty()) {
        return result; // 이름이 빠진 멤버 접근은 구문 오류(S0207)가 이미 있다
    }

    // S0326: 비슷한 멤버 이름을 제시한다(spaces는 찾은 이름 공간들).
    auto unknown = [&](StringView owner, Span<const uint64> spaces, uint32 receiverType) noexcept {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' has no member '{}'.", owner, name);
        StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
        const StringView similar = SimilarMember(spaces, receiverType, name);
        if (similar.IsEmpty()) {
            fix.Append("Check the member name.");
        } else {
            fix.AppendFormat("Did you mean '{}'?", similar);
        }

        if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
            Report(context, DiagnosticCode::UnknownMember, node, message.View(),
                   "Members come from the type and its base types.", fix.View());
        }
    };
    auto typeOwner = [&](uint32 type) noexcept {
        if (HasIncompleteMembers(type)) {
            return;
        }

        Array<uint64> spaces(allocator_);
        uint32 at = SymbolOf(type);
        for (uint32 step = 0; at != NoSymbol && step < compilation_.symbols_.Size(); ++step) {
            if (auto pushed = spaces.TryPushBack(SpaceKey(NameSpaceKind::Members, at)); !pushed) {
                binder_.error_ = pushed.Error();
                return;
            }

            at = compilation_.symbols_[at].base;
        }

        StringBuilder<CompilationMessageInlineBytes> text(allocator_);
        AppendDisplayType(text, type);
        unknown(text.View(), spaces.View(), type);
    };
    // 함수가 아닌 멤버에 붙은 타입 인자는 S0320이다.
    auto finish = [&](const ExpressionValue& value) noexcept {
        if (value.kind != K::Function && value.kind != K::Error &&
            RejectTypeArguments(context, tree.Child(node, 1), name)) {
            return ExpressionValue{};
        }

        return value;
    };

    // 이름 공간·타입 이름은 null이 아니다(3.10절 "멤버 접근").
    if (chained && (receiver.kind == K::Namespace || receiver.kind == K::TypeName)) {
        Report(context, DiagnosticCode::NotOptional, node,
               "'?.' needs a value whose type has '?"
               "'.",
               "A type name or a namespace is never null.", "Write '.' instead of '?.'.");
        return result;
    }

    // 자기 멤버는 `this.` 없이 쓴다(괄호로 감싼 `this`도 같다).
    uint32 object = objectNode;
    while (object != NoNode && tree.Node(object).kind == SyntaxKind::Parenthesized) {
        object = tree.Child(object, 0);
    }

    if (object != NoNode && tree.Node(object).kind == SyntaxKind::This && !chained) {
        Report(context, DiagnosticCode::ThisMisuse, node,
               "A member of this class is written without 'this.'.",
               "Inside a class its own members are used by name.", "Remove 'this.'.");
        return result;
    }

    uint32 member = NoSymbol;
    if (receiver.kind == K::Namespace) {
        const Symbol& entry = compilation_.symbols_[receiver.symbol];
        if (entry.kind == SymbolKind::RpcPrefix) {
            return RpcMember(context, node, receiver.symbol, name);
        }

        if (entry.kind == SymbolKind::Alias) {
            member = binder_.FindName(SpaceKey(NameSpaceKind::ModuleTop, entry.index), name);
        } else {
            member = binder_.FindName(SpaceKey(NameSpaceKind::Members, receiver.symbol), name);
        }

        if (member == NoSymbol) {
            const uint64 space = entry.kind == SymbolKind::Alias
                                     ? SpaceKey(NameSpaceKind::ModuleTop, entry.index)
                                     : SpaceKey(NameSpaceKind::Members, receiver.symbol);
            unknown(entry.name, Span<const uint64>(&space, 1), NoType);
            return result;
        }

        binder_.SetNodeSymbol(context.module, node, member);
        return finish(ValueOfSymbol(context, member, NoType, false));
    }

    if (receiver.kind == K::TypeName) {
        const uint32 type = TypeOfSymbol(receiver.symbol);
        member = FindMember(type, name);
        if (member == NoSymbol) {
            typeOwner(type);
            return result;
        }

        const Symbol& entry = compilation_.symbols_[member];
        if (entry.kind != SymbolKind::EnumCase && !IsStaticFunction(entry)) {
            ReportStaticInstance(context, node, name, NoType);
            return result;
        }

        binder_.SetNodeSymbol(context.module, node, member);
        return finish(ValueOfSymbol(context, member, type, false));
    }

    // 타입 없는 상수는 기본 타입(int·float)의 값이고, `null`의 멤버는 S0417이다.
    uint32 type = receiver.type;
    if (receiver.kind == K::UntypedInt || receiver.kind == K::UntypedFloat ||
        receiver.kind == K::Null) {
        type = Materialize(context, receiver, objectNode);
        if (type == NoType) {
            return result;
        }
    } else if (receiver.kind != K::Typed) {
        RequireValue(context, receiver, objectNode);
        return result;
    }

    if (chained) {
        if (!IsOptional(type)) {
            Report(context, DiagnosticCode::NotOptional, node,
                   "'?.' needs a value whose type has '?"
                   "'.",
                   "This value is never null.", "Write '.' instead of '?.'.");
            return result;
        }

        type = NonOptional(type);
    } else if (IsOptional(type) && receiver.stale) {
        ReportStaleRead(context, objectNode);
        return result;
    } else if (IsOptional(type)) {
        Report(context, DiagnosticCode::MayBeNull, objectNode,
               "The value may be null, so its members cannot be used directly.",
               "A value whose type has '?' is null when the object is gone or missing.",
               "Use '?.', 'if (let x = value) { ... }' or '?"
               "?'.");
        return result;
    }

    member = FindMember(type, name);
    if (member == NoSymbol) {
        // Actor·World 확장 이벤트(3.12절 — 이 모듈이나 직접 import한 모듈의 것)다.
        member = MemberExtensionEvent(context, node, type, name);
        if (member == AmbiguousExtension) {
            return result;
        }
    }

    if (member == NoSymbol || !MemberAvailable(member, type)) {
        typeOwner(type);
        return result;
    }

    const Symbol& entry = compilation_.symbols_[member];
    if (entry.kind == SymbolKind::EnumCase || IsStaticFunction(entry)) {
        ReportStaticInstance(context, node, name, NonOptional(type));
        return result;
    }

    binder_.SetNodeSymbol(context.module, node, member);
    result = ValueOfSymbol(context, member, type, true);
    result.chained = chained;
    if (chained && result.kind == K::Typed) {
        result.type = MakeOptional(result.type);
    }

    return finish(result);
}

void TypeChecker::ReportStaticInstance(const CheckContext& context, uint32 node, StringView name,
                                       uint32 valueType) noexcept {
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    if (valueType == NoType) {
        message.AppendFormat("'{}' belongs to a value of the type, not to the type itself.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::StaticInstanceMismatch, node, message.View(),
                   "Only enum cases and static functions are used through a type name.",
                   "Use a value of the type.");
        }

        return;
    }

    message.AppendFormat("'{}' belongs to the type, not to a value.", name);
    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
    fix.Append("Write '");
    AppendDisplayType(fix, valueType);
    fix.Append(".").Append(name).Append("'.");
    if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
        Report(context, DiagnosticCode::StaticInstanceMismatch, node, message.View(),
               "Enum cases and static functions are used through the type name.", fix.View());
    }
}

StringView TypeChecker::SimilarMember(Span<const uint64> spaces, uint32 receiverType,
                                      StringView name) const noexcept {
    StringView best;
    usize bestDistance = MaxMemberSuggestionDistance + 1;
    binder_.names_.ForEach([&](const NameKey& key, const uint32& symbol) noexcept {
        bool inSpace = false;
        for (const uint64 space : spaces) {
            inSpace = inSpace || key.space == space;
        }

        if (!inSpace || (receiverType != NoType && !MemberAvailable(symbol, receiverType))) {
            return;
        }

        const usize distance = EditDistance(name, key.name, MaxMemberSuggestionDistance,
                                            EditDistanceCase::AsciiInsensitive);
        if (distance < bestDistance ||
            (distance == bestDistance && distance <= MaxMemberSuggestionDistance &&
             StringView::CompareBytes(key.name, best) < 0)) {
            bestDistance = distance;
            best = key.name;
        }
    });
    return best;
}

void TypeChecker::ReportStaleRead(const CheckContext& context, uint32 objectNode) noexcept {
    uint32 name = objectNode;
    while (name != NoNode &&
           compilation_.trees_[context.module].Node(name).kind == SyntaxKind::Parenthesized) {
        name = compilation_.trees_[context.module].Child(name, 0);
    }

    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
    const StringView text = TokenText(context.module, name);
    message.AppendFormat("'{}' may have been destroyed while waiting, so its members cannot be "
                         "used directly.",
                         text);
    fix.Append("Check it again with 'if (let x = ");
    fix.Append(text);
    fix.Append(") { ... }', or use '?.'.");
    if (binder_.Succeeded(message) && binder_.Succeeded(fix)) {
        Report(context, DiagnosticCode::MayBeNull, objectNode, message.View(),
               "After 'await', a local name that refers to an object has a type with '?': the "
               "object can be destroyed while the function waits.",
               fix.View());
    }
}

ExpressionValue TypeChecker::CheckCall(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept {
    // 재귀 경로(`f().g().h() ...`)의 frame에는 부르는 값만 둔다(CheckBinary 참고).
    const ExpressionValue callee =
        Check(context, compilation_.trees_[context.module].Child(node, 0), NoType);
    return CallValue(context, node, expected, callee);
}

ExpressionValue TypeChecker::CallValue(const CheckContext& context, uint32 node, uint32 expected,
                                       const ExpressionValue& callee) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = JudgeCall(context, node, expected, callee);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::JudgeCall(const CheckContext& context, uint32 node, uint32 expected,
                                       const ExpressionValue& callee) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 calleeNode = tree.Child(node, 0);
    const SyntaxKind calleeKind = tree.Node(calleeNode).kind;
    const uint32 typeArguments = calleeKind == SyntaxKind::Identifier ? tree.Child(calleeNode, 0)
                                 : calleeKind == SyntaxKind::Member   ? tree.Child(calleeNode, 1)
                                                                      : NoNode;
    ExpressionValue result;
    auto checkArguments = [&]() noexcept {
        const uint32 list = tree.Child(node, 1);
        if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
            return;
        }

        for (const uint32 argument : tree.Children(list)) {
            if (tree.Node(argument).kind == SyntaxKind::Argument) {
                static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
            }
        }
    };

    switch (callee.kind) {
    case K::Function:
        // RPC는 대상 접두로만 부른다(3.15절 S1004).
        if (rules_ && !RpcTarget(callee.symbol).IsEmpty()) {
            uint32 object = tree.Node(calleeNode).kind == SyntaxKind::Member
                                ? tree.Child(calleeNode, 0)
                                : NoNode;
            while (object != NoNode && tree.Node(object).kind == SyntaxKind::Parenthesized) {
                object = tree.Child(object, 0);
            }

            const uint32 prefix =
                object == NoNode ? NoSymbol : compilation_.NodeSymbol(context.module, object);
            if (prefix == NoSymbol || compilation_.symbols_[prefix].kind != SymbolKind::RpcPrefix) {
                StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
                fix.AppendFormat("Write '{}.{}(...)'.", RpcTarget(callee.symbol),
                                 compilation_.symbols_[callee.symbol].name);
                if (binder_.Succeeded(fix)) {
                    Report(context, DiagnosticCode::RpcCall, calleeNode,
                           "An RPC is called with its target prefix.",
                           "The prefix shows at the call that the function runs elsewhere.",
                           fix.View());
                }
            }
        }

        // 값 타입 경로를 바꾸는 변경 메서드는 그 경로가 쓸 수 있어야 한다(3.11절).
        if (callee.instance && IsMutatingMember(callee.symbol) &&
            tree.Node(calleeNode).kind == SyntaxKind::Member) {
            CheckWritablePath(context, tree.Child(calleeNode, 0), node);
        }

        return CallFunction(context, node, callee, typeArguments);
    case K::Literal:
        return CheckLiteral(context, node, callee.symbol, expected);
    case K::TypeName: {
        const Symbol& entry = compilation_.symbols_[callee.symbol];
        if (entry.kind == SymbolKind::BuiltinType) {
            const NumericInfo numeric = Numeric(TypeOfSymbol(callee.symbol));
            if (numeric.kind != NumericKind::None || entry.name == "bool" ||
                entry.name == "string") {
                return Convert(context, node, callee.symbol);
            }
        }

        if (entry.kind == SymbolKind::PreludeType &&
            LanguageNames()[entry.index].kind == LanguageNameKind::TypeLiteral) {
            return CheckLiteral(context, node, callee.symbol, expected);
        }

        return Construct(context, node, callee.symbol);
    }
    case K::Error:
        checkArguments();
        return result;
    case K::Event:
        if (rules_) {
            // 이벤트는 호출하지 않는다(3.12절 S0812).
            Report(context, DiagnosticCode::EventCall, calleeNode,
                   "An event is raised with 'emit', not called.",
                   "Writing an event as a call would neither raise it nor wait for it.",
                   "Write 'emit Source.Event(...)', or 'await Source.Event' to wait for it.");
            checkArguments();
            return result;
        }

        [[fallthrough]];
    default:
        if (RequireValue(context, callee, calleeNode) && callee.kind != K::Function) {
            Report(context, DiagnosticCode::NotCallable, calleeNode,
                   "This value is not a function.",
                   "Only functions, types and validated literals are called.",
                   "Remove the parentheses.");
        }

        checkArguments();
        return result;
    }
}

ExpressionValue TypeChecker::CallFunction(const CheckContext& context, uint32 node,
                                          const ExpressionValue& callee,
                                          uint32 typeArgumentList) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const Symbol entry = compilation_.symbols_[callee.symbol];
    ExpressionValue result;
    const uint32 list = tree.Child(node, 1);
    Array<uint32> arguments(allocator_);
    if (list != NoNode && tree.Node(list).kind == SyntaxKind::List) {
        for (const uint32 argument : tree.Children(list)) {
            if (tree.Node(argument).kind != SyntaxKind::Argument) {
                continue;
            }

            if (auto pushed = arguments.TryPushBack(argument); !pushed) {
                binder_.error_ = pushed.Error();
                return result;
            }
        }
    }

    // 함수 호출은 위치 인자만 쓴다.
    for (const uint32 argument : arguments) {
        if (tree.Child(argument, 0) != NoNode) {
            Report(context, DiagnosticCode::ArgumentForm, argument,
                   "A function call uses positional arguments.",
                   "Named arguments are for creating values ('Type(Field: value)').",
                   "Remove the argument name.");
            return result;
        }
    }

    if (entry.signature == NoSignature) {
        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    const Signature signature = compilation_.signatures_[entry.signature];
    // 타입 인자(제네릭 함수는 명시한다 — 추론하지 않는다).
    Array<uint32> typeArguments(allocator_);
    if (typeArgumentList != NoNode && tree.Node(typeArgumentList).kind == SyntaxKind::List) {
        for (const uint32 typeNode : tree.Children(typeArgumentList)) {
            if (auto pushed =
                    typeArguments.TryPushBack(compilation_.NodeType(context.module, typeNode));
                !pushed) {
                binder_.error_ = pushed.Error();
                return result;
            }
        }
    }

    const uint32 arity = entry.arity;
    if (typeArguments.Size() != arity) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (arity == 0) {
            message.AppendFormat("'{}' takes no type arguments.", entry.name);
        } else {
            message.AppendFormat("'{}' needs {} type argument{}.", entry.name, arity,
                                 arity == 1 ? "" : "s");
        }

        if (binder_.Succeeded(message)) {
            Report(context,
                   arity == 0 ? DiagnosticCode::TypeArgumentCount
                              : DiagnosticCode::TypeArgumentRule,
                   tree.Child(node, 0), message.View(),
                   "Type arguments of a generic function are written, not inferred.",
                   arity == 0 ? StringView("Remove the type arguments.")
                              : StringView("Write them, as in 'Find<Health>()'."));
        }

        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    // schema 타입 매개변수의 제약.
    if (entry.kind == SymbolKind::EngineFunction && arity != 0) {
        const SchemaFunction& source =
            compilation_.schemas_[entry.index].schema.Functions()[entry.item];
        for (uint32 position = 0; position < arity; ++position) {
            const uint32 parameter = binder_.FindName(
                SpaceKey(NameSpaceKind::Parameters, callee.symbol),
                compilation_.schemas_[entry.index].schema.Text(
                    compilation_.schemas_[entry.index]
                        .schema.TypeParameters()[source.firstTypeParameter + position]
                        .name));
            const uint32 constraint =
                parameter == NoSymbol ? NoSymbol : compilation_.symbols_[parameter].base;
            const uint32 argument = typeArguments[position];
            if (constraint != NoSymbol && argument != NoType &&
                !IsDerived(argument, TypeOfSymbol(constraint))) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("Type argument '");
                AppendDisplayType(message, argument);
                message.AppendFormat("' is not derived from '{}'.",
                                     compilation_.symbols_[constraint].name);
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::TypeArgumentRule, tree.Child(node, 0),
                           message.View(), "The type argument must satisfy the constraint.",
                           "Use a type derived from the constraint.");
                }

                return result;
            }
        }
    }

    // `Assets.Find`·`Assets.LoadAll`의 T는 에셋 타입이다(3.10절 "호출").
    if (entry.kind == SymbolKind::LanguageFunction && entry.owner != NoSymbol &&
        compilation_.symbols_[entry.owner].name == "Assets" && typeArguments.Size() == 1 &&
        typeArguments[0] != NoType && !IsAssetType(typeArguments[0])) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("Type argument '");
        AppendDisplayType(message, typeArguments[0]);
        message.Append("' is not an asset type.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::TypeArgumentRule, tree.Child(node, 0), message.View(),
                   "Assets are loaded by asset type.",
                   "Use an asset type, as in 'Find<SoundAsset>'.");
        }

        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    // 대기 1개 이상(All·Race, 3.10절·6.1절 SD41).
    if ((entry.flags & SymbolFlags::Variadic) != 0) {
        if (arguments.IsEmpty()) {
            Report(context, DiagnosticCode::ArgumentCount, node,
                   "'All' and 'Race' need at least one wait.", "", "Pass the waits.");
            return result;
        }

        for (const uint32 argument : arguments) {
            const uint32 valueNode = tree.Child(argument, 1);
            const ExpressionValue value = Check(context, valueNode, NoType);
            const bool voidWait =
                (value.kind == K::Wait && value.type == voidType_) || value.kind == K::Event;
            const bool request = value.kind == K::Typed && IsLanguage(value.type, "LoadRequest") &&
                                 tree.Node(valueNode).kind == SyntaxKind::Identifier &&
                                 value.symbol != NoSymbol &&
                                 compilation_.symbols_[value.symbol].kind == SymbolKind::Local;
            if (value.kind != K::Error && !voidWait && !request) {
                Report(context, DiagnosticCode::Unawaited, valueNode,
                       "'All' and 'Race' take waits without results or load requests held in "
                       "local variables.",
                       "A result passed here would be lost.",
                       "Keep the request in a local variable first, as in 'let req = "
                       "Icon.Load();'.");
            }
        }

        result.kind = K::Wait;
        result.type = voidType_;
        return result;
    }

    if (arguments.Size() != signature.parameterCount) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' takes {} argument{}, but {} {} given.", entry.name,
                             signature.parameterCount, signature.parameterCount == 1 ? "" : "s",
                             arguments.Size(), arguments.Size() == 1 ? "is" : "are");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ArgumentCount, node, message.View(), "",
                   "Pass one argument for each parameter.");
        }

        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    // Math 오버로딩: 자리표시 매개변수의 인자를 같은 타입으로 맞춘다(3.1절).
    uint32 chosen = NoType;
    bool overloaded = false;
    Array<ExpressionValue> values(allocator_);
    for (uint32 index = 0; index < signature.parameterCount; ++index) {
        const uint32 parameter =
            compilation_.signatureParameters_[signature.firstParameter + index];
        overloaded = overloaded || ContainsPlaceholder(parameter);
    }

    if (overloaded) {
        bool anyFloat = false;
        for (uint32 index = 0; index < signature.parameterCount; ++index) {
            const uint32 parameter =
                compilation_.signatureParameters_[signature.firstParameter + index];
            const ExpressionValue value =
                Check(context, tree.Child(arguments[index], 1),
                      ContainsPlaceholder(parameter) ? NoType : parameter);
            if (auto pushed = values.TryPushBack(value); !pushed) {
                binder_.error_ = pushed.Error();
                return result;
            }

            if (ContainsPlaceholder(parameter)) {
                anyFloat = anyFloat || value.kind == K::UntypedFloat;
                if (value.kind == K::Typed && chosen == NoType) {
                    chosen = value.type;
                }
            }
        }

        if (chosen == NoType) {
            // 모두 상수: 실수 상수가 있거나 자리표시가 int를 받지 않으면 float, 아니면 int(3.10절).
            chosen = anyFloat ? floatType_ : intType_;
            for (uint32 index = 0; index < signature.parameterCount && chosen == intType_;
                 ++index) {
                const uint32 parameter =
                    compilation_.signatureParameters_[signature.firstParameter + index];
                if (ContainsPlaceholder(parameter) &&
                    !PlaceholderAccepts(SymbolOf(parameter), intType_)) {
                    chosen = floatType_;
                }
            }
        }

        for (uint32 index = 0; index < signature.parameterCount; ++index) {
            const uint32 parameter =
                compilation_.signatureParameters_[signature.firstParameter + index];
            if (!ContainsPlaceholder(parameter)) {
                continue;
            }

            const uint32 placeholder = SymbolOf(parameter);
            if (!PlaceholderAccepts(placeholder, chosen)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("'{}' has no version for '", entry.name);
                AppendDisplayType(message, chosen);
                message.Append("'.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::TypeMismatch, tree.Child(arguments[index], 1),
                           message.View(),
                           "The function has versions for some types only (appendix B).",
                           "Convert the argument to a type it accepts.");
                }

                return result;
            }
        }
    }

    bool failed = false;
    // `Assert`의 첫 인자는 조건이다(3.10절 "조건", S0408).
    const bool assertion = entry.kind == SymbolKind::PreludeValue && entry.name == "Assert";
    for (uint32 index = 0; index < signature.parameterCount; ++index) {
        const uint32 parameter =
            Substitute(compilation_.signatureParameters_[signature.firstParameter + index],
                       callee.receiver, typeArguments.View(), chosen);
        const uint32 valueNode = tree.Child(arguments[index], 1);
        const ExpressionValue value =
            overloaded
                ? values[index]
                : Check(context, valueNode, parameter == NoType ? UnresolvedType : parameter);
        if (parameter == NoType) {
            continue;
        }

        if (assertion && index == 0 && value.kind != K::Error &&
            !(value.kind == K::Typed && value.type == boolType_)) {
            if (RequireValue(context, value, valueNode)) {
                Report(context, DiagnosticCode::ConditionNotBool, valueNode,
                       "The condition of 'Assert' must be 'bool'.",
                       "Conditions are never made from other types silently.",
                       "Compare it, as in 'x != null'.");
            }

            failed = true;
            continue;
        }

        if (value.kind == K::Error || Coerce(context, value, parameter, valueNode) == NoType) {
            failed = true;
        }
    }

    // 인자에 진단이 있으면 호출 결과도 해석하지 못한 것이다(연쇄 방지, 3.10절).
    if (failed) {
        return result;
    }

    // `Math.Div`의 0(S0504).
    if (entry.kind == SymbolKind::LanguageFunction && entry.name == "Div" &&
        entry.owner != NoSymbol && compilation_.symbols_[entry.owner].name == "Math" &&
        values.Size() == 2 && ReportZeroDivisor(context, values[1], tree.Child(arguments[1], 1))) {
        return result;
    }
    const uint32 returnType =
        Substitute(signature.returnType, callee.receiver, typeArguments.View(), chosen);
    if (signature.isAsync) {
        if (returnType == NoType) {
            return result; // 해석하지 못한 반환 타입(연쇄 방지)
        }

        // `?.`로 부른 대기는 수신자가 null이면 바로 끝나므로 결과가 `R?`다.
        result.kind = K::Wait;
        result.type =
            callee.chained && returnType != voidType_ ? MakeOptional(returnType) : returnType;
        return result;
    }

    if (returnType == voidType_) {
        result.kind = K::Void;
        return result;
    }

    if (returnType == NoType) {
        return result;
    }

    result.kind = K::Typed;
    result.type = callee.chained ? MakeOptional(returnType) : returnType;
    return result;
}

ExpressionValue TypeChecker::Convert(const CheckContext& context, uint32 node,
                                     uint32 typeSymbol) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 target = TypeOfSymbol(typeSymbol);
    ExpressionValue result;
    const uint32 list = tree.Child(node, 1);
    const Span<const uint32> arguments =
        list != NoNode ? tree.Children(list) : Span<const uint32>();
    if (Numeric(target).kind == NumericKind::None) {
        Report(context, DiagnosticCode::InvalidConversion, node,
               "Only numbers are converted with a type name.",
               "'bool' and 'string' have no conversion calls.",
               "Use 'Parse.Int(text)' or interpolation \"${value}\".");
        return result;
    }

    if (arguments.Size() != 1 || tree.Child(arguments[0], 0) != NoNode) {
        Report(context,
               arguments.Size() != 1 ? DiagnosticCode::ArgumentCount : DiagnosticCode::ArgumentForm,
               node, "A conversion takes one positional argument.", "",
               "Write it as in 'float(x)'.");
        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    const uint32 valueNode = tree.Child(arguments[0], 1);
    const ExpressionValue value = Check(context, valueNode, NoType);
    if (value.kind == K::Error || !RequireValue(context, value, valueNode)) {
        return result;
    }

    const bool targetFloat = Numeric(target).kind == NumericKind::Float;
    if (value.kind == K::UntypedFloat ||
        (value.kind == K::Typed && Numeric(value.type).kind == NumericKind::Float)) {
        if (!targetFloat) {
            Report(context, DiagnosticCode::FloatToIntConversion, node,
                   "A floating-point value is not converted to an integer type directly.",
                   "Rounding must be chosen explicitly.",
                   "Use 'Math.FloorToInt', 'CeilToInt', 'RoundToInt' or 'TruncToInt'.");
            return result;
        }
    }

    if (value.kind == K::UntypedInt || value.kind == K::UntypedFloat) {
        if (Coerce(context, value, target, valueNode) == NoType) {
            return result;
        }
    } else if (value.kind != K::Typed || Numeric(value.type).kind == NumericKind::None) {
        Report(context, DiagnosticCode::InvalidConversion, valueNode,
               "Only a number can be converted to a number type.", "",
               "Use 'Parse.Int(text)' or 'Parse.Float(text)' for strings.");
        return result;
    } else if (value.constant.kind == ConstantValue::Kind::Integer &&
               !ConstantFits(value.constant, true, target)) {
        // 정해진 상수의 변환도 컴파일 때 판정한다(3.10절 "변환").
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("The constant does not fit in '");
        AppendDisplayType(message, target);
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::IntegerLiteralTooLarge, valueNode, message.View(),
                   "A constant must fit in the type of its place.",
                   "Use a value in range or a wider type.");
        }

        return result;
    } else if (value.constant.kind == ConstantValue::Kind::Float &&
               !ConstantFits(value.constant, false, target)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("The constant does not fit in '");
        AppendDisplayType(message, target);
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::IntegerLiteralTooLarge, valueNode, message.View(),
                   "A constant must fit in the type of its place.",
                   "Use a value in range or a wider type.");
        }

        return result;
    }

    result.kind = K::Typed;
    result.type = target;
    // 상수의 변환 결과도 상수다(정수 → 실수는 실수 값, float32는 반올림).
    result.constant = value.constant;
    if (result.constant.kind == ConstantValue::Kind::Integer && targetFloat) {
        const double real = result.constant.negative
                                ? -static_cast<double>(result.constant.magnitude)
                                : static_cast<double>(result.constant.magnitude);
        result.constant = ConstantValue{};
        result.constant.kind = ConstantValue::Kind::Float;
        result.constant.real = real;
    }

    if (result.constant.kind == ConstantValue::Kind::Float && Numeric(target).bits == 32) {
        result.constant.real = static_cast<double>(static_cast<float>(result.constant.real));
    }

    return result;
}

ExpressionValue TypeChecker::Construct(const CheckContext& context, uint32 node,
                                       uint32 typeSymbol) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const Symbol entry = compilation_.symbols_[typeSymbol];
    const uint32 type = TypeOfSymbol(typeSymbol);
    ExpressionValue result;
    const uint32 list = tree.Child(node, 1);
    const Span<const uint32> arguments = list != NoNode && tree.Node(list).kind == SyntaxKind::List
                                             ? tree.Children(list)
                                             : Span<const uint32>();
    auto notCallable = [&](StringView message) noexcept {
        Report(context, DiagnosticCode::NotCallable, tree.Child(node, 0), message,
               "Values of this type are created by the engine.",
               "Use the engine API that creates it.");
        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }
    };

    // 위치 인자 생성: 벡터·quat.
    uint32 positional = 0;
    if (entry.kind == SymbolKind::BuiltinType) {
        if (entry.name == "vec2") {
            positional = 2;
        } else if (entry.name == "vec3") {
            positional = 3;
        } else if (entry.name == "vec4" || entry.name == "quat") {
            positional = 4;
        } else {
            notCallable("This type has no constructor.");
            return result;
        }

        // 이름 지정 인자(S0404)를 인자 수(S0403)보다 먼저 본다. 진단은 호출 하나에 하나다.
        for (const uint32 argument : arguments) {
            if (tree.Child(argument, 0) != NoNode) {
                Report(context, DiagnosticCode::ArgumentForm, argument,
                       "Vectors and quaternions take positional components.", "",
                       "Remove the argument name.");
                for (const uint32 each : arguments) {
                    static_cast<void>(Check(context, tree.Child(each, 1), UnresolvedType));
                }

                return result;
            }
        }

        if (arguments.Size() != positional) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' takes {} components.", entry.name, positional);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ArgumentCount, node, message.View(), "",
                       "Pass one floating-point value per component.");
            }

            for (const uint32 argument : arguments) {
                static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
            }

            return result;
        }

        bool failed = false;
        for (const uint32 argument : arguments) {
            const uint32 valueNode = tree.Child(argument, 1);
            const ExpressionValue value = Check(context, valueNode, floatType_);
            if (value.kind == K::Error || Coerce(context, value, floatType_, valueNode) == NoType) {
                failed = true;
            }
        }

        if (failed) {
            return result;
        }

        result.kind = K::Typed;
        result.type = type;
        return result;
    }

    // 이름 지정 생성: struct, schema struct, component, Rotator·Color·Transform.
    bool allowed = entry.kind == SymbolKind::Struct;
    if (entry.kind == SymbolKind::PreludeType) {
        allowed = entry.name == "Rotator" || entry.name == "Color" || entry.name == "Transform" ||
                  entry.name == "TagSet";
    } else if (entry.kind == SymbolKind::EngineType) {
        const SchemaType& source = compilation_.schemas_[entry.index].schema.Types()[entry.item];
        if (source.kind == SchemaTypeKind::Struct) {
            if (!source.constructible) {
                notCallable("This engine struct cannot be created by scripts.");
                return result;
            }

            allowed = true;
        } else {
            allowed = source.kind == SchemaTypeKind::Object &&
                      binder_.EngineChainHas(typeSymbol, binder_.component_);
        }
    } else if (entry.kind == SymbolKind::Class) {
        // 기반을 해석하지 못한 class는 component인지 모른다(연쇄 방지).
        if (HasIncompleteMembers(type)) {
            for (const uint32 argument : arguments) {
                static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
            }

            return result;
        }

        uint32 at = typeSymbol;
        while (at != NoSymbol && compilation_.symbols_[at].kind == SymbolKind::Class) {
            at = compilation_.symbols_[at].base;
        }

        allowed = at != NoSymbol && binder_.EngineChainHas(at, binder_.component_);
    }

    if (!allowed) {
        notCallable("Values of this type cannot be created with a call.");
        return result;
    }

    // 인자마다 이름·값을 검사한다. 위치 인자의 S0404는 첫 위치 인자 한 곳이고, 인자에 진단이
    // 있으면 생성 결과는 해석하지 못한 것이다(3.10절 연쇄 방지).
    Array<uint32> given(allocator_);
    bool failed = false;
    bool positionalReported = false;
    for (const uint32 argument : arguments) {
        const uint32 nameNode = tree.Child(argument, 0);
        const uint32 valueNode = tree.Child(argument, 1);
        if (nameNode == NoNode) {
            if (!positionalReported) {
                Report(context, DiagnosticCode::ArgumentForm, argument,
                       "A value is created with named arguments.",
                       "Each argument names the field it sets.",
                       "Write it as in 'HitInfo(Damage: 1)'.");
                positionalReported = true;
            }

            failed = true;
            static_cast<void>(Check(context, valueNode, UnresolvedType));
            continue;
        }

        const StringView fieldName = binder_.NodeText(context.module, nameNode);
        const uint32 field = FindMember(type, fieldName);
        const SymbolKind fieldKind =
            field == NoSymbol ? SymbolKind::Local : compilation_.symbols_[field].kind;
        if (fieldKind != SymbolKind::Field && fieldKind != SymbolKind::EngineField &&
            fieldKind != SymbolKind::LanguageField) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' has no field '{}'.", entry.name, fieldName);
            if (!HasIncompleteMembers(type) && binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::UnknownMember, nameNode, message.View(),
                       "A named argument sets a field of the created value.",
                       "Check the field name.");
            }

            failed = true;
            static_cast<void>(Check(context, valueNode, UnresolvedType));
            continue;
        }

        bool repeated = false;
        for (const uint32 earlier : given) {
            repeated = repeated || earlier == field;
        }

        if (repeated) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Field '{}' is set twice.", fieldName);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ArgumentForm, nameNode, message.View(), "",
                       "Remove one of them.");
            }

            failed = true;
            static_cast<void>(Check(context, valueNode, UnresolvedType));
            continue;
        }

        if (auto pushed = given.TryPushBack(field); !pushed) {
            binder_.error_ = pushed.Error();
            return result;
        }

        binder_.SetNodeSymbol(context.module, nameNode, field);
        const uint32 fieldType =
            fieldKind == SymbolKind::Field
                ? FieldType(field)
                : Substitute(compilation_.symbols_[field].type, type, Span<const uint32>(), NoType);
        const ExpressionValue value = Check(context, valueNode, fieldType);
        if (value.kind == K::Error ||
            (fieldType != NoType && Coerce(context, value, fieldType, valueNode) == NoType)) {
            failed = true;
        }
    }

    if (failed) {
        return result;
    }

    // 기본값이 없는 필드는 지정해야 한다(Sparks struct·schema struct). 빠진 필드를 모아 호출
    // 하나에 S0404 하나로 알린다(선언 순서).
    StringBuilder<CompilationMessageInlineBytes> missing(allocator_);
    uint32 missingCount = 0;
    auto requireField = [&](uint32 field, StringView fieldName, uint32 fieldType,
                            bool hasInitializer) noexcept {
        if (hasInitializer || fieldType == NoType || HasDefault(fieldType)) {
            return;
        }

        for (const uint32 earlier : given) {
            if (earlier == field) {
                return;
            }
        }

        missing.Append(missingCount == 0 ? "'" : ", '").Append(fieldName).Append("'");
        ++missingCount;
    };
    // Sparks 선언의 필드(선언 순서).
    auto requireDeclared = [&](uint32 declaration, uint32 membersChild) noexcept {
        const Symbol& owner = compilation_.symbols_[declaration];
        const SyntaxTree& ownerTree = compilation_.trees_[owner.module];
        const uint32 members = ownerTree.Child(owner.node, membersChild);
        if (members == NoNode || ownerTree.Node(members).kind != SyntaxKind::List) {
            return;
        }

        for (const uint32 member : ownerTree.Children(members)) {
            if (ownerTree.Node(member).kind != SyntaxKind::Field) {
                continue;
            }

            const uint32 field = compilation_.NodeSymbol(owner.module, ownerTree.Child(member, 2));
            if (field != NoSymbol) {
                requireField(field, compilation_.symbols_[field].name, FieldType(field),
                             ownerTree.Child(member, 4) != NoNode);
            }
        }
    };
    if (entry.kind == SymbolKind::Struct) {
        requireDeclared(typeSymbol, 3);
    } else if (entry.kind == SymbolKind::Class) {
        // component: Sparks가 선언한 필드만(기반 class부터 — 엔진 필드는 엔진이 정한다).
        Array<uint32> chain(allocator_);
        uint32 at = typeSymbol;
        for (uint32 step = 0; at != NoSymbol && step < compilation_.symbols_.Size() &&
                              compilation_.symbols_[at].kind == SymbolKind::Class;
             ++step) {
            if (auto pushed = chain.TryPushBack(at); !pushed) {
                binder_.error_ = pushed.Error();
                return result;
            }

            at = compilation_.symbols_[at].base;
        }

        for (usize index = chain.Size(); index > 0; --index) {
            requireDeclared(chain[index - 1], 4);
        }
    } else if (entry.kind == SymbolKind::EngineType) {
        const BindingSchema& schema = compilation_.schemas_[entry.index].schema;
        const SchemaType& source = schema.Types()[entry.item];
        if (source.kind == SchemaTypeKind::Struct) {
            for (uint32 at = source.firstField; at < source.firstField + source.fieldCount; ++at) {
                const StringView fieldName = schema.Text(schema.Fields()[at].name);
                const uint32 field =
                    binder_.FindName(SpaceKey(NameSpaceKind::Members, typeSymbol), fieldName);
                if (field != NoSymbol) {
                    requireField(field, fieldName, compilation_.symbols_[field].type, false);
                }
            }
        }
    }

    if (missingCount != 0) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append(missingCount == 1 ? "Field " : "Fields ")
            .Append(missing.View())
            .Append(" must be given.");
        if (binder_.Succeeded(missing) && binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ArgumentForm, node, message.View(),
                   "A field without a default value (assets, Soft, Row, Tag and InputAction "
                   "fields without '?') must be set when the value is created.",
                   "Pass it, as in 'Field: value'.");
        }

        return result;
    }

    result.kind = K::Typed;
    result.type = type;
    return result;
}

ExpressionValue TypeChecker::CheckLiteral(const CheckContext& context, uint32 node, uint32 literal,
                                          uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const StringView name = compilation_.symbols_[literal].name;
    ExpressionValue result;
    const uint32 list = tree.Child(node, 1);
    const Span<const uint32> arguments = list != NoNode && tree.Node(list).kind == SyntaxKind::List
                                             ? tree.Children(list)
                                             : Span<const uint32>();
    // 인자 수(S0403)를 먼저 본다: `Loc`은 key 하나 이상, 나머지는 하나.
    const bool isLoc = name == "Loc";
    if (arguments.IsEmpty() || (!isLoc && arguments.Size() != 1)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat(isLoc ? "'{}' takes a key." : "'{}' takes one argument.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ArgumentCount, node, message.View(), "",
                   isLoc ? StringView("Write it as in 'Loc(\"Hud.Title\")'.")
                         : StringView("Pass one string literal."));
        }

        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), UnresolvedType));
        }

        return result;
    }

    // 첫 인자: 보간 없는 문자열 리터럴.
    const uint32 first = tree.Child(arguments[0], 1);
    bool literalText = false;
    if (first != NoNode && tree.Child(arguments[0], 0) == NoNode) {
        const SyntaxNode& value = tree.Node(first);
        literalText = value.kind == SyntaxKind::VerbatimString;
        if (value.kind == SyntaxKind::String) {
            literalText = true;
            for (const uint32 part : tree.Children(first)) {
                literalText = literalText && tree.Node(part).kind != SyntaxKind::Interpolation;
            }
        }
    }

    if (!literalText) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' takes a string literal without interpolation.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ValidatedLiteralForm, first, message.View(),
                   "The compiler checks the name against the engine data, so it must be "
                   "written in the source.",
                   "Write it as in 'Tag(\"Item.Key\")', or use 'Tag.Find(text)' at run time.");
        }

        for (usize index = 1; index < arguments.Size(); ++index) {
            static_cast<void>(Check(context, tree.Child(arguments[index], 1), UnresolvedType));
        }

        return result;
    }

    Record(context, first, stringType_);
    const uint64 language = SpaceKey(NameSpaceKind::Language, 0);

    uint32 type = NoType;
    if (name == "Tag") {
        type = TypeOfSymbol(binder_.FindName(language, "Tag"));
    } else if (name == "Action") {
        type = TypeOfSymbol(binder_.FindName(language, "InputAction"));
    } else if (isLoc) {
        // key 뒤는 이름 지정 인자다: 위치 인자는 첫 것 한 곳에 S0404, 같은 이름은 뒤 것에 S0404,
        // 값은 보간할 수 있는 타입(S0419). 인자에 진단이 있으면 결과는 해석하지 못한 것이다.
        bool failed = false;
        bool positionalReported = false;
        for (usize index = 1; index < arguments.Size(); ++index) {
            const uint32 nameNode = tree.Child(arguments[index], 0);
            const uint32 valueNode = tree.Child(arguments[index], 1);
            if (nameNode == NoNode) {
                if (!positionalReported) {
                    Report(context, DiagnosticCode::ArgumentForm, arguments[index],
                           "The arguments after the key of 'Loc' are named.",
                           "Each argument fills the named place in the catalog text.",
                           "Write it as in 'Loc(\"Hud.Hp\", Current: hp)'.");
                    positionalReported = true;
                }

                failed = true;
                static_cast<void>(Check(context, valueNode, UnresolvedType));
                continue;
            }

            const StringView argumentName = binder_.NodeText(context.module, nameNode);
            bool repeated = false;
            for (usize earlier = 1; earlier < index; ++earlier) {
                const uint32 earlierName = tree.Child(arguments[earlier], 0);
                repeated =
                    repeated || (earlierName != NoNode &&
                                 binder_.NodeText(context.module, earlierName) == argumentName);
            }

            if (repeated) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("Argument '{}' is given twice.", argumentName);
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::ArgumentForm, nameNode, message.View(), "",
                           "Remove one of them.");
                }

                failed = true;
                static_cast<void>(Check(context, valueNode, UnresolvedType));
                continue;
            }

            const ExpressionValue value = Check(context, valueNode, NoType);
            if (value.kind == K::Error || !RequireValue(context, value, valueNode)) {
                failed = true;
                continue;
            }

            const uint32 valueType = Materialize(context, value, valueNode);
            if (valueType == NoType) {
                failed = true;
            } else if (!IsInterpolatable(valueType)) {
                Report(context, DiagnosticCode::InterpolationType, valueNode,
                       "This value cannot be shown in a localized text.",
                       "Loc arguments take the types that interpolation shows.",
                       "Pass a field of it or check it for null first.");
                failed = true;
            }
        }

        if (failed) {
            return result;
        }

        type = TypeOfSymbol(binder_.FindName(language, "Text"));
    } else {
        // Asset·Row·CVar: 기대 타입이 정한다.
        const uint32 target =
            expected == NoType || expected == UnresolvedType ? NoType : NonOptional(expected);
        bool fits = false;
        if (name == "Asset") {
            fits = target != NoType && (IsAssetType(target) || IsLanguage(target, "Soft"));
        } else if (name == "Row") {
            fits = IsLanguage(target, "Row");
        } else {
            fits = IsLanguage(target, "CVarRef");
        }

        if (!fits && expected == UnresolvedType) {
            return result; // 기대 타입을 모르는 자리(앞선 오류)
        }

        if (!fits && target != NoType) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}(...)' cannot be used as '", name);
            AppendDisplayType(message, expected);
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::TypeMismatch, node, message.View(),
                       name == "Asset" ? StringView("'Asset(...)' is an asset type or a Soft<T>.")
                       : name == "Row" ? StringView("'Row(...)' is a Row<T>.")
                                       : StringView("'CVar(...)' is a CVarRef<T>."),
                       "Use a value of the expected type.");
            }

            return result;
        }

        if (!fits) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}(...)' needs an expected type here.", name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::NoExpectedType, node, message.View(),
                       name == "Asset"
                           ? StringView("The expected type tells whether it is a hard reference "
                                        "(an asset type) or a soft reference (Soft<T>).")
                       : name == "Row" ? StringView("The expected type Row<T> tells the table.")
                                       : StringView("The expected type CVarRef<T> tells the "
                                                    "value type."),
                       "Declare the type, as in 'var mesh: Soft<Mesh> = Asset(\"/Game/Mesh\");'.");
            }

            return result;
        }

        type = target;
    }

    if (type == NoType) {
        return result;
    }

    result.kind = K::Typed;
    result.type = type;
    return result;
}

} // namespace rocks::sparks::detail
