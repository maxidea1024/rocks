#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "TypeChecker.h"

// 멤버 접근(S0326·S0412·S0414·S0425)과 호출(S0402–S0404·S0411·S0422), 생성·숫자 변환(S0502·S0508),
// 검증 리터럴(S0417·S0424)이다. 규칙: Docs/Design/SparksLanguage.ko.md 3.10절, 부록 B 시그니처.

namespace rocks::sparks::detail {

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

bool TypeChecker::MemberAvailable(uint32 member, uint32 receiver) const noexcept {
    const Symbol& entry = compilation_.symbols_[member];
    if (entry.kind != SymbolKind::LanguageField && entry.kind != SymbolKind::LanguageFunction) {
        return true;
    }

    const uint32 element = receiver == NoType ? NoType : Argument(receiver, 0);
    switch (static_cast<PreludeCondition>(entry.index)) {
    case PreludeCondition::Equatable:
        return IsEquatable(element);
    case PreludeCondition::Ordered:
        return IsOrdered(element);
    case PreludeCondition::ValueElement:
        return !IsReference(element);
    case PreludeCondition::ObjectElement:
        return IsReference(element);
    case PreludeCondition::NotVoid:
        return element != voidType_;
    default:
        return true;
    }
}

ExpressionValue TypeChecker::CheckMember(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 objectNode = tree.Child(node, 0);
    const bool chained = (tree.Node(node).flags & SyntaxFlags::NullConditional) != 0;
    const StringView name = TokenText(context.module, node);
    const ExpressionValue receiver = Check(context, objectNode, NoType);
    ExpressionValue result;
    if (receiver.kind == K::Error) {
        return result;
    }

    auto unknown = [&](StringView owner) noexcept {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' has no member '{}'.", owner, name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::UnknownMember, node, message.View(),
                   "Members come from the type and its base types.",
                   "Check the member name.");
        }
    };
    auto typeOwner = [&](uint32 type) noexcept {
        StringBuilder<CompilationMessageInlineBytes> text(allocator_);
        static_cast<void>(compilation_.AppendTypeText(text, type));
        unknown(text.View());
    };

    uint32 member = NoSymbol;
    if (receiver.kind == K::Namespace) {
        const Symbol& entry = compilation_.symbols_[receiver.symbol];
        if (entry.kind == SymbolKind::Alias) {
            member = binder_.FindName(SpaceKey(NameSpaceKind::ModuleTop, entry.index), name);
        } else {
            member = binder_.FindName(SpaceKey(NameSpaceKind::Members, receiver.symbol), name);
        }

        if (member == NoSymbol) {
            unknown(entry.name);
            return result;
        }

        binder_.SetNodeSymbol(context.module, node, member);
        return ValueOfSymbol(context, member, NoType, false);
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
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' belongs to a value of the type, not to the type itself.",
                                 name);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::StaticInstanceMismatch, node, message.View(),
                       "Only enum cases and static functions are used through a type name.",
                       "Use a value of the type.");
            }

            return result;
        }

        binder_.SetNodeSymbol(context.module, node, member);
        return ValueOfSymbol(context, member, type, false);
    }

    if (receiver.kind != K::Typed) {
        RequireValue(context, receiver, objectNode);
        return result;
    }

    uint32 type = receiver.type;
    if (chained) {
        if (!IsOptional(type)) {
            Report(context, DiagnosticCode::NotOptional, node,
                   "'?.' needs a value whose type has '?" "'.", "This value is never null.",
                   "Write '.' instead of '?.'.");
            return result;
        }

        type = NonOptional(type);
    } else if (IsOptional(type)) {
        Report(context, DiagnosticCode::MayBeNull, objectNode,
               "The value may be null, so its members cannot be used directly.",
               "A value whose type has '?' is null when the object is gone or missing.",
               "Use '?.', 'if (let x = value) { ... }' or '?" "?'.");
        return result;
    }

    member = FindMember(type, name);
    if (member == NoSymbol || !MemberAvailable(member, type)) {
        typeOwner(type);
        return result;
    }

    const Symbol& entry = compilation_.symbols_[member];
    if (entry.kind == SymbolKind::EnumCase || IsStaticFunction(entry)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' belongs to the type, not to a value.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::StaticInstanceMismatch, node, message.View(),
                   "Enum cases and static functions are used through the type name.",
                   "Write the type name before it.");
        }

        return result;
    }

    binder_.SetNodeSymbol(context.module, node, member);
    result = ValueOfSymbol(context, member, type, true);
    result.chained = chained;
    if (chained && result.kind == K::Typed) {
        result.type = MakeOptional(result.type);
    }

    return result;
}

ExpressionValue TypeChecker::CheckCall(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 calleeNode = tree.Child(node, 0);
    const SyntaxKind calleeKind = tree.Node(calleeNode).kind;
    const uint32 typeArguments = calleeKind == SyntaxKind::Identifier ? tree.Child(calleeNode, 0)
                                 : calleeKind == SyntaxKind::Member  ? tree.Child(calleeNode, 1)
                                                                     : NoNode;
    const ExpressionValue callee = Check(context, calleeNode, NoType);
    ExpressionValue result;
    auto checkArguments = [&]() noexcept {
        const uint32 list = tree.Child(node, 1);
        if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
            return;
        }

        for (const uint32 argument : tree.Children(list)) {
            if (tree.Node(argument).kind == SyntaxKind::Argument) {
                static_cast<void>(Check(context, tree.Child(argument, 1), NoType));
            }
        }
    };

    switch (callee.kind) {
    case K::Function:
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
    default:
        if (RequireValue(context, callee, calleeNode) && callee.kind != K::Function) {
            Report(context, DiagnosticCode::NotCallable, calleeNode, "This value is not a function.",
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
            static_cast<void>(Check(context, tree.Child(argument, 1), NoType));
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
            static_cast<void>(Check(context, tree.Child(argument, 1), NoType));
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
                static_cast<void>(compilation_.AppendTypeText(message, argument));
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
            static_cast<void>(Check(context, tree.Child(argument, 1), NoType));
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
            const ExpressionValue value = Check(context, tree.Child(arguments[index], 1),
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
            chosen = anyFloat ? floatType_ : intType_;
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
                static_cast<void>(compilation_.AppendTypeText(message, chosen));
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
    for (uint32 index = 0; index < signature.parameterCount; ++index) {
        const uint32 parameter = Substitute(
            compilation_.signatureParameters_[signature.firstParameter + index], callee.receiver,
            typeArguments.View(), chosen);
        const uint32 valueNode = tree.Child(arguments[index], 1);
        const ExpressionValue value =
            overloaded ? values[index] : Check(context, valueNode, parameter);
        if (parameter == NoType) {
            continue;
        }

        if (Coerce(context, value, parameter, valueNode) == NoType && value.kind != K::Error) {
            failed = true;
        }
    }

    static_cast<void>(failed);
    const uint32 returnType =
        Substitute(signature.returnType, callee.receiver, typeArguments.View(), chosen);
    if (signature.isAsync) {
        result.kind = K::Wait;
        result.type = returnType == NoType ? voidType_ : returnType;
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
        Report(context, DiagnosticCode::ArgumentCount, node,
               "A conversion takes one positional argument.", "", "Write it as in 'float(x)'.");
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
    }

    result.kind = K::Typed;
    result.type = target;
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
    const Span<const uint32> arguments =
        list != NoNode && tree.Node(list).kind == SyntaxKind::List ? tree.Children(list)
                                                                   : Span<const uint32>();
    auto notCallable = [&](StringView message) noexcept {
        Report(context, DiagnosticCode::NotCallable, tree.Child(node, 0), message,
               "Values of this type are created by the engine.",
               "Use the engine API that creates it.");
        for (const uint32 argument : arguments) {
            static_cast<void>(Check(context, tree.Child(argument, 1), NoType));
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

        if (arguments.Size() != positional) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("'{}' takes {} components.", entry.name, positional);
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ArgumentCount, node, message.View(), "",
                       "Pass one floating-point value per component.");
            }

            return result;
        }

        for (const uint32 argument : arguments) {
            if (tree.Child(argument, 0) != NoNode) {
                Report(context, DiagnosticCode::ArgumentForm, argument,
                       "Vectors and quaternions take positional components.", "",
                       "Remove the argument name.");
                return result;
            }

            const uint32 valueNode = tree.Child(argument, 1);
            static_cast<void>(
                Coerce(context, Check(context, valueNode, floatType_), floatType_, valueNode));
        }

        result.kind = K::Typed;
        result.type = type;
        return result;
    }

    // 이름 지정 생성: struct, schema struct, component, Rotator·Color·Transform.
    bool allowed = entry.kind == SymbolKind::Struct;
    if (entry.kind == SymbolKind::PreludeType) {
        allowed = entry.name == "Rotator" || entry.name == "Color" || entry.name == "Transform";
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

    Array<uint32> given(allocator_);
    for (const uint32 argument : arguments) {
        const uint32 nameNode = tree.Child(argument, 0);
        const uint32 valueNode = tree.Child(argument, 1);
        if (nameNode == NoNode) {
            Report(context, DiagnosticCode::ArgumentForm, argument,
                   "A value is created with named arguments.",
                   "Each argument names the field it sets.",
                   "Write it as in 'HitInfo(Damage: 1)'.");
            static_cast<void>(Check(context, valueNode, NoType));
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
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::UnknownMember, nameNode, message.View(),
                       "A named argument sets a field of the created value.",
                       "Check the field name.");
            }

            static_cast<void>(Check(context, valueNode, NoType));
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

            continue;
        }

        if (auto pushed = given.TryPushBack(field); !pushed) {
            binder_.error_ = pushed.Error();
            return result;
        }

        binder_.SetNodeSymbol(context.module, nameNode, field);
        const uint32 fieldType = fieldKind == SymbolKind::Field
                                     ? FieldType(field)
                                     : Substitute(compilation_.symbols_[field].type, type,
                                                  Span<const uint32>(), NoType);
        const ExpressionValue value = Check(context, valueNode, fieldType);
        if (fieldType != NoType) {
            static_cast<void>(Coerce(context, value, fieldType, valueNode));
        }
    }

    // 기본값이 없는 필드는 지정해야 한다(Sparks struct·schema struct).
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

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Field '{}' must be given.", fieldName);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ArgumentForm, node, message.View(),
                   "The field has no default value (assets, Soft, Row, Tag and InputAction "
                   "fields without '?').",
                   "Pass it, as in 'Field: value'.");
        }
    };
    if (entry.kind == SymbolKind::Struct) {
        const SyntaxTree& owner = compilation_.trees_[entry.module];
        const uint32 members = owner.Child(entry.node, 3);
        if (members != NoNode && owner.Node(members).kind == SyntaxKind::List) {
            for (const uint32 member : owner.Children(members)) {
                if (owner.Node(member).kind != SyntaxKind::Field) {
                    continue;
                }

                const uint32 field = compilation_.NodeSymbol(entry.module, owner.Child(member, 2));
                if (field != NoSymbol) {
                    requireField(field, compilation_.symbols_[field].name, FieldType(field),
                                 owner.Child(member, 4) != NoNode);
                }
            }
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

    result.kind = K::Typed;
    result.type = type;
    return result;
}

ExpressionValue TypeChecker::CheckLiteral(const CheckContext& context, uint32 node,
                                          uint32 literal, uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const StringView name = compilation_.symbols_[literal].name;
    ExpressionValue result;
    const uint32 list = tree.Child(node, 1);
    const Span<const uint32> arguments =
        list != NoNode && tree.Node(list).kind == SyntaxKind::List ? tree.Children(list)
                                                                   : Span<const uint32>();
    // 첫 인자: 보간 없는 문자열 리터럴.
    const uint32 first = arguments.IsEmpty() ? NoNode : tree.Child(arguments[0], 1);
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
            Report(context, DiagnosticCode::ValidatedLiteralForm, first == NoNode ? node : first,
                   message.View(),
                   "The compiler checks the name against the engine data, so it must be "
                   "written in the source.",
                   "Write it as in 'Tag(\"Item.Key\")', or use 'Tag.Find(text)' at run time.");
        }

        for (usize index = 1; index < arguments.Size(); ++index) {
            static_cast<void>(Check(context, tree.Child(arguments[index], 1), NoType));
        }

        return result;
    }

    Record(context, first, stringType_);
    const uint64 language = SpaceKey(NameSpaceKind::Language, 0);
    const bool isLoc = name == "Loc";
    if (!isLoc && arguments.Size() != 1) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("'{}' takes one argument.", name);
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ArgumentCount, node, message.View(), "",
                   "Remove the extra arguments.");
        }

        return result;
    }

    uint32 type = NoType;
    if (name == "Tag") {
        type = TypeOfSymbol(binder_.FindName(language, "Tag"));
    } else if (name == "Action") {
        type = TypeOfSymbol(binder_.FindName(language, "InputAction"));
    } else if (isLoc) {
        for (usize index = 1; index < arguments.Size(); ++index) {
            const uint32 valueNode = tree.Child(arguments[index], 1);
            if (tree.Child(arguments[index], 0) == NoNode) {
                Report(context, DiagnosticCode::ArgumentForm, arguments[index],
                       "The arguments after the key of 'Loc' are named.",
                       "Each argument fills the named place in the catalog text.",
                       "Write it as in 'Loc(\"Hud.Hp\", Current: hp)'.");
                continue;
            }

            const ExpressionValue value = Check(context, valueNode, NoType);
            if (value.kind == K::Error || !RequireValue(context, value, valueNode)) {
                continue;
            }

            const uint32 valueType = Materialize(context, value, valueNode);
            if (valueType != NoType && !IsInterpolatable(valueType)) {
                Report(context, DiagnosticCode::InterpolationType, valueNode,
                       "This value cannot be shown in a localized text.",
                       "Loc arguments take the types that interpolation shows.",
                       "Pass a field of it or check it for null first.");
            }
        }

        type = TypeOfSymbol(binder_.FindName(language, "Text"));
    } else {
        // Asset·Row·CVar: 기대 타입이 정한다.
        const uint32 target = NonOptional(expected);
        bool fits = false;
        if (name == "Asset") {
            fits = target != NoType && (IsAssetType(target) || IsLanguage(target, "Soft"));
        } else if (name == "Row") {
            fits = IsLanguage(target, "Row");
        } else {
            fits = IsLanguage(target, "CVarRef");
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
