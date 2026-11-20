#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "TypeChecker.h"

// 선언 초기값과 본문의 문장이다(Docs/Design/SparksLanguage.ko.md 3.10절): 지역 선언의 타입,
// 조건(S0408), `for`, 대입의 타입, `switch`(S0415·S0416), 반환(S0409·S0410)과 "끝나는 문장".

namespace rocks::sparks::detail {

namespace {

// 필드·const 추론 상태다.
constexpr uint8 NotInferred = 0;
constexpr uint8 Inferring = 1;
constexpr uint8 Inferred = 2;

bool SameConstant(const ConstantValue& left, const ConstantValue& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }

    switch (left.kind) {
    case ConstantValue::Kind::Integer:
        return left.negative == right.negative && left.magnitude == right.magnitude;
    case ConstantValue::Kind::Float:
        return left.real == right.real;
    case ConstantValue::Kind::Bool:
        return left.boolean == right.boolean;
    case ConstantValue::Kind::String:
        return left.text == right.text;
    case ConstantValue::Kind::Enum:
        return left.symbol == right.symbol;
    default:
        return false;
    }
}

} // namespace

void TypeChecker::Run() noexcept {
    Compilation& c = compilation_;
    const uint64 language = SpaceKey(NameSpaceKind::Language, 0);
    boolType_ = TypeOfSymbol(binder_.FindName(language, "bool"));
    intType_ = TypeOfSymbol(binder_.FindName(language, "int"));
    floatType_ = TypeOfSymbol(binder_.FindName(language, "float"));
    stringType_ = TypeOfSymbol(binder_.FindName(language, "string"));
    voidType_ = binder_.voidType_;
    errorEnum_ = binder_.FindName(SpaceKey(NameSpaceKind::Engine, 0), "Error");
    if (auto resized = inference_.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    if (auto resized = constants_.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (usize index = 0; index < inference_.Size(); ++index) {
        inference_[index] = NotInferred;
    }

    for (uint32 module = 0; module < c.modules_.Size() && binder_.error_ == ErrorCode::None;
         ++module) {
        if (c.nodeBase_[module] != NoNode) {
            CheckModule(module);
        }
    }
}

uint32 TypeChecker::FieldType(uint32 field) noexcept {
    Compilation& c = compilation_;
    if (field >= inference_.Size() || inference_[field] == Inferred) {
        return c.symbols_[field].type;
    }

    if (inference_[field] == Inferring) {
        return NoType; // 서로를 기다리는 초기값(상수식 규칙은 LC2d가 본다)
    }

    inference_[field] = Inferring;
    const Symbol entry = c.symbols_[field];
    const SyntaxTree& tree = c.trees_[entry.module];
    const uint32 initializer = tree.Child(entry.node, 4);
    CheckContext context;
    context.module = entry.module;
    context.owner = entry.owner;
    if (entry.owner != NoSymbol && c.symbols_[entry.owner].kind == SymbolKind::Class) {
        context.thisType = TypeOfSymbol(entry.owner);
    }

    uint32 type = entry.type;
    if (initializer != NoNode) {
        const ExpressionValue value = Check(context, initializer, type);
        if (type != NoType) {
            static_cast<void>(Coerce(context, value, type, initializer));
        } else if (tree.Child(entry.node, 3) == NoNode) {
            type = Materialize(context, value, initializer);
        }
    }

    c.symbols_[field].type = type;
    inference_[field] = Inferred;
    return type;
}

ConstantValue TypeChecker::ConstOf(uint32 symbol) noexcept {
    Compilation& c = compilation_;
    if (symbol >= inference_.Size()) {
        return ConstantValue{};
    }

    if (inference_[symbol] != NotInferred) {
        return constants_[symbol];
    }

    inference_[symbol] = Inferring;
    const Symbol entry = c.symbols_[symbol];
    const SyntaxTree& tree = c.trees_[entry.module];
    CheckContext context;
    context.module = entry.module;
    const uint32 initializer = tree.Child(entry.node, 4);
    const ExpressionValue value = Check(context, initializer, entry.type);
    ConstantValue constant;
    if (entry.type != NoType && Coerce(context, value, entry.type, initializer) != NoType) {
        constant = value.constant;
    }

    constants_[symbol] = constant;
    inference_[symbol] = Inferred;
    return constant;
}

void TypeChecker::CheckModule(uint32 module) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const uint32 declarations = tree.Child(tree.Root(), 1);
    CheckContext moduleContext;
    moduleContext.module = module;
    for (const uint32 node : tree.Children(declarations)) {
        if (binder_.error_ != ErrorCode::None) {
            return;
        }

        const SyntaxNode& entry = tree.Node(node);
        const uint32 nameNode = entry.childCount > 2 ? tree.Child(node, 2) : NoNode;
        const uint32 symbol = nameNode == NoNode ? NoSymbol : c.NodeSymbol(module, nameNode);
        switch (entry.kind) {
        case SyntaxKind::Const:
            if (symbol != NoSymbol) {
                static_cast<void>(ConstOf(symbol));
            }

            break;
        case SyntaxKind::Function:
            CheckFunction(moduleContext, node, symbol);
            break;
        case SyntaxKind::Class:
        case SyntaxKind::Struct: {
            const bool isClass = entry.kind == SyntaxKind::Class;
            CheckContext typeContext = moduleContext;
            typeContext.owner = symbol;
            if (isClass && symbol != NoSymbol) {
                typeContext.thisType = TypeOfSymbol(symbol);
            }

            const uint32 members = tree.Child(node, isClass ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                break;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& memberEntry = tree.Node(member);
                if (memberEntry.kind == SyntaxKind::Field) {
                    const uint32 field = c.NodeSymbol(module, tree.Child(member, 2));
                    if (field != NoSymbol) {
                        static_cast<void>(FieldType(field));
                    }
                } else if (memberEntry.kind == SyntaxKind::Function) {
                    CheckFunction(typeContext, member, c.NodeSymbol(module, tree.Child(member, 2)));
                } else if (memberEntry.kind == SyntaxKind::Handler) {
                    CheckFunction(typeContext, member, NoSymbol);
                }
            }

            break;
        }
        default:
            break;
        }
    }
}

void TypeChecker::CheckFunction(const CheckContext& outer, uint32 function,
                                uint32 symbol) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[outer.module];
    const bool handler = tree.Node(function).kind == SyntaxKind::Handler;
    CheckContext context = outer;
    context.returnKnown = true;
    context.returnType = voidType_;
    if (!handler) {
        const uint32 signature = symbol == NoSymbol ? NoSignature : c.symbols_[symbol].signature;
        context.returnType =
            signature == NoSignature ? NoType : c.signatures_[signature].returnType;
        context.returnKnown = context.returnType != NoType;
        if (context.returnKnown && IsLanguage(context.returnType, "Result") &&
            !IsOptional(context.returnType)) {
            context.resultFunction = true;
            context.resultValue = Argument(context.returnType, 0);
            if (IsOptional(context.resultValue)) {
                Report(context, DiagnosticCode::TypeMismatch, tree.Child(function, 4),
                       "'Result' cannot hold a value whose type has '?'.",
                       "A missing value is an error case, not a successful null.",
                       "Return 'Result<T>' and an 'Error' value instead of null.");
            }
        }
    }

    const uint32 block = tree.Child(function, handler ? 4 : 5);
    CheckStatement(context, block);
    if (!context.returnKnown || context.returnType == voidType_ ||
        (context.resultFunction && context.resultValue == voidType_) || block == NoNode ||
        tree.Node(block).kind != SyntaxKind::Block) {
        return;
    }

    if (!Terminates(outer.module, block)) {
        const SourceSpan span = tree.Node(block).span;
        ReportAt(context, DiagnosticCode::MissingReturn,
                 SourceSpan{span.end == 0 ? 0 : span.end - 1, span.end},
                 "The function can reach its end without returning a value.",
                 "Every path of a function with a return type ends with 'return'.",
                 "Add a 'return' at the end.");
    }
}

void TypeChecker::CheckReturn(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 valueNode = tree.Child(node, 0);
    if (!context.returnKnown) {
        if (valueNode != NoNode) {
            static_cast<void>(Check(context, valueNode, NoType));
        }

        return;
    }

    const bool voidLike = context.returnType == voidType_;
    const bool resultVoid = context.resultFunction && context.resultValue == voidType_;
    if (valueNode == NoNode) {
        if (!voidLike && !resultVoid) {
            Report(context, DiagnosticCode::ReturnValue, node,
                   "The function must return a value.", "Its return type is not void.",
                   "Write 'return value;'.");
        }

        return;
    }

    if (voidLike) {
        static_cast<void>(Check(context, valueNode, NoType));
        Report(context, DiagnosticCode::ReturnValue, valueNode,
               "This function returns no value.", "Its return type is void.",
               "Write 'return;'.");
        return;
    }

    if (context.resultFunction) {
        const ExpressionValue value = Check(context, valueNode, resultVoid ? NoType : context.resultValue);
        // `Error` 값은 실패다.
        if (value.kind == K::Typed && errorEnum_ != NoSymbol &&
            SymbolOf(value.type) == errorEnum_ && !IsOptional(value.type)) {
            return;
        }

        if (resultVoid) {
            if (value.kind != K::Error) {
                Report(context, DiagnosticCode::ReturnValue, valueNode,
                       "A 'Result<void>' function returns 'return;' or an 'Error' value.", "",
                       "Write 'return;' for success.");
            }

            return;
        }

        static_cast<void>(Coerce(context, value, context.resultValue, valueNode));
        return;
    }

    const ExpressionValue value = Check(context, valueNode, context.returnType);
    static_cast<void>(Coerce(context, value, context.returnType, valueNode));
}

bool TypeChecker::Terminates(uint32 module, uint32 node) const noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    if (node == NoNode) {
        return false;
    }

    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::Return:
        return true;
    case SyntaxKind::Block:
        return entry.childCount != 0 && Terminates(module, tree.Child(node, entry.childCount - 1));
    case SyntaxKind::If:
        return tree.Child(node, 2) != NoNode && Terminates(module, tree.Child(node, 1)) &&
               Terminates(module, tree.Child(node, 2));
    case SyntaxKind::Switch: {
        const uint64 key = (uint64(module) << 32) | node;
        bool exhaustive = tree.Child(node, 2) != NoNode;
        for (const uint64 recorded : exhaustiveSwitches_) {
            exhaustive = exhaustive || recorded == key;
        }

        if (!exhaustive) {
            return false;
        }

        const uint32 cases = tree.Child(node, 1);
        if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
            for (const uint32 item : tree.Children(cases)) {
                if (tree.Node(item).kind == SyntaxKind::SwitchCase &&
                    !Terminates(module, tree.Child(item, 1))) {
                    return false;
                }
            }
        }

        const uint32 fallback = tree.Child(node, 2);
        return fallback == NoNode || Terminates(module, tree.Child(fallback, 0));
    }
    default:
        return false;
    }
}

void TypeChecker::CheckSwitch(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 subjectNode = tree.Child(node, 0);
    const ExpressionValue subject = Check(context, subjectNode, NoType);
    uint32 type = NoType;
    if (subject.kind != K::Error && RequireValue(context, subject, subjectNode)) {
        type = Materialize(context, subject, subjectNode);
        if (type != NoType && !IsEquatable(NonOptional(type))) {
            Report(context, DiagnosticCode::NotComparable, subjectNode,
                   "A switch needs a value that can be compared with '=='.",
                   "Cases are matched with '=='.", "Use 'if' for this value.");
            type = NoType;
        }
    }

    const uint32 cases = tree.Child(node, 1);
    const uint32 fallback = tree.Child(node, 2);
    Array<ConstantValue> seen(allocator_);
    if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
        for (const uint32 item : tree.Children(cases)) {
            if (tree.Node(item).kind != SyntaxKind::SwitchCase) {
                continue;
            }

            const uint32 values = tree.Child(item, 0);
            if (values != NoNode && tree.Node(values).kind == SyntaxKind::List) {
                for (const uint32 valueNode : tree.Children(values)) {
                    const ExpressionValue value = Check(context, valueNode, type);
                    if (type == NoType || value.kind == K::Error) {
                        continue;
                    }

                    if (Coerce(context, value, type, valueNode) == NoType) {
                        continue;
                    }

                    if (value.constant.kind == ConstantValue::Kind::None) {
                        Report(context, DiagnosticCode::TypeMismatch, valueNode,
                               "A case value must be a constant.",
                               "Cases are enum cases, number or string literals and constants.",
                               "Use 'if' to compare with a value computed at run time.");
                        continue;
                    }

                    bool repeated = false;
                    for (const ConstantValue& earlier : seen) {
                        repeated = repeated || SameConstant(earlier, value.constant);
                    }

                    if (repeated) {
                        Report(context, DiagnosticCode::DuplicateCase, valueNode,
                               "This case value appears twice.", "", "Remove the repeated value.");
                        continue;
                    }

                    if (auto pushed = seen.TryPushBack(value.constant); !pushed) {
                        binder_.error_ = pushed.Error();
                        return;
                    }
                }
            }

            CheckStatement(context, tree.Child(item, 1));
        }
    }

    if (fallback != NoNode && tree.Node(fallback).kind == SyntaxKind::SwitchDefault) {
        CheckStatement(context, tree.Child(fallback, 0));
    }

    if (type == NoType) {
        return;
    }

    const uint32 subjectSymbol = SymbolOf(NonOptional(type));
    const bool enumSubject = IsEnumType(NonOptional(type)) && !IsOptional(type);
    const bool engineError = subjectSymbol == errorEnum_;
    if (enumSubject && !engineError) {
        if (fallback != NoNode) {
            Report(context, DiagnosticCode::SwitchCoverage, fallback,
                   "A switch on an enum has no 'default'.",
                   "Every case is listed, so a new case added later is reported here.",
                   "Remove 'default' and list the cases.");
            return;
        }

        // 빠진 case(선언 순서)를 메시지에 적는다.
        StringBuilder<CompilationMessageInlineBytes> missing(allocator_);
        uint32 missingCount = 0;
        const Span<const Symbol> symbols = c.symbols_.View();
        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (symbols[at].kind != SymbolKind::EnumCase || symbols[at].owner != subjectSymbol) {
                continue;
            }

            bool covered = false;
            for (const ConstantValue& earlier : seen) {
                covered = covered || (earlier.kind == ConstantValue::Kind::Enum && earlier.symbol == at);
            }

            if (!covered) {
                missing.Append(missingCount == 0 ? "" : ", ").Append(symbols[at].name);
                ++missingCount;
            }
        }

        if (missingCount != 0) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("The switch does not handle {}.", missing.View());
            if (binder_.Succeeded(missing) && binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::SwitchCoverage, subjectNode, message.View(),
                       "A switch on an enum handles every case.", "Add the missing cases.");
            }

            return;
        }

        if (auto pushed = exhaustiveSwitches_.TryPushBack((uint64(context.module) << 32) | node);
            !pushed) {
            binder_.error_ = pushed.Error();
        }

        return;
    }

    if (fallback == NoNode) {
        Report(context, DiagnosticCode::SwitchCoverage, subjectNode,
               engineError ? StringView("A switch on 'Error' needs 'default'.")
                           : StringView("This switch needs 'default'."),
               engineError ? StringView("The engine may add error cases later.")
                           : StringView("Only a switch on an enum lists every value."),
               "Add 'default: { }' at the end.");
    }
}

void TypeChecker::CheckStatement(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    if (node == NoNode || binder_.error_ != ErrorCode::None) {
        return;
    }

    const SyntaxNode& entry = tree.Node(node);
    auto condition = [&](uint32 expression) noexcept {
        const ExpressionValue value = Check(context, expression, boolType_);
        if (value.kind == K::Error) {
            return;
        }

        if (!(value.kind == K::Typed && value.type == boolType_)) {
            if (RequireValue(context, value, expression)) {
                Report(context, DiagnosticCode::ConditionNotBool, expression,
                       "A condition must be 'bool'.",
                       "Conditions are never made from other types silently.",
                       "Compare it, as in 'x != null' or 'count > 0'.");
            }
        }
    };

    switch (entry.kind) {
    case SyntaxKind::Block:
        for (const uint32 statement : tree.Children(node)) {
            CheckStatement(context, statement);
        }

        break;
    case SyntaxKind::LocalVariable: {
        const uint32 nameNode = tree.Child(node, 0);
        const uint32 local = nameNode == NoNode ? NoSymbol : c.NodeSymbol(context.module, nameNode);
        const uint32 typeNode = tree.Child(node, 1);
        const uint32 declared = typeNode == NoNode ? NoType : c.NodeType(context.module, typeNode);
        const uint32 valueNode = tree.Child(node, 2);
        const uint32 exit = tree.Child(node, 3);
        const bool exits = exit != NoNode && tree.Node(exit).kind == SyntaxKind::ExitReturn;
        const ExpressionValue value =
            Check(context, valueNode, exits ? MakeOptional(declared) : declared);
        uint32 type = NoType;
        if (exits) {
            if (value.kind != K::Error && !(value.kind == K::Typed && IsOptional(value.type))) {
                if (RequireValue(context, value, valueNode)) {
                    Report(context, DiagnosticCode::NotOptional, valueNode,
                           "'?? return' needs a value whose type has '?'.",
                           "It leaves the function when the value is null.",
                           "Remove '?? return'.");
                }
            } else if (value.kind == K::Typed) {
                type = NonOptional(value.type);
                if (declared != NoType && !IsAssignable(type, declared)) {
                    ReportMismatch(context, valueNode, type, declared);
                }
            }

            CheckReturn(context, exit);
        } else if (declared != NoType) {
            static_cast<void>(Coerce(context, value, declared, valueNode));
        } else {
            type = Materialize(context, value, valueNode);
        }

        if (local != NoSymbol && declared == NoType) {
            c.symbols_[local].type = type;
        }

        break;
    }
    case SyntaxKind::If: {
        const uint32 test = tree.Child(node, 0);
        if (test != NoNode && tree.Node(test).kind == SyntaxKind::IfLet) {
            const uint32 valueNode = tree.Child(test, 1);
            const ExpressionValue value = Check(context, valueNode, NoType);
            const uint32 binder = c.NodeSymbol(context.module, tree.Child(test, 0));
            if (value.kind == K::Typed && IsOptional(value.type) &&
                NonOptional(value.type) != boolType_) {
                if (binder != NoSymbol) {
                    c.symbols_[binder].type = NonOptional(value.type);
                }
            } else if (value.kind != K::Error && RequireValue(context, value, valueNode)) {
                const bool optionalBool = value.kind == K::Typed && IsOptional(value.type);
                Report(context, DiagnosticCode::NotOptional, valueNode,
                       optionalBool ? StringView("'if (let)' cannot test a 'bool?'.")
                                    : StringView("'if (let)' needs a value whose type has '?'."),
                       optionalBool ? StringView("It would be unclear whether it tests for null "
                                                 "or for true.")
                                    : StringView("This value is never null."),
                       optionalBool ? StringView("Write 'e ?? false' to test for true or 'e != "
                                                 "null' to test for a value.")
                                    : StringView("Use 'let x = e;' or 'if (e)'."));
            }
        } else {
            condition(test);
        }

        CheckStatement(context, tree.Child(node, 1));
        CheckStatement(context, tree.Child(node, 2));
        break;
    }
    case SyntaxKind::For: {
        const uint32 iterableNode = tree.Child(node, 2);
        const uint32 endNode = tree.Child(node, 3);
        const uint32 first = c.NodeSymbol(context.module, tree.Child(node, 0));
        const uint32 secondNode = tree.Child(node, 1);
        const uint32 second = secondNode == NoNode ? NoSymbol : c.NodeSymbol(context.module, secondNode);
        const bool two = secondNode != NoNode;
        uint32 firstType = NoType;
        uint32 secondType = NoType;
        if (endNode != NoNode) {
            // 범위 `a..<b`: 같은 정수 타입.
            const ExpressionValue start = Check(context, iterableNode, NoType);
            const uint32 hint = start.kind == K::Typed ? start.type : NoType;
            const ExpressionValue end = Check(context, endNode, hint);
            uint32 type = hint;
            if (type == NoType && end.kind == K::Typed) {
                type = end.type;
            }

            if (type == NoType) {
                type = intType_;
            }

            const NumericInfo numeric = Numeric(type);
            if (numeric.kind != NumericKind::Signed && numeric.kind != NumericKind::Unsigned) {
                if (start.kind != K::Error && end.kind != K::Error) {
                    Report(context, DiagnosticCode::TypeMismatch, iterableNode,
                           "A range goes over integers.", "", "Use integer bounds.");
                }
            } else if (Coerce(context, start, type, iterableNode) != NoType &&
                       Coerce(context, end, type, endNode) != NoType) {
                firstType = type;
            }

            if (two) {
                Report(context, DiagnosticCode::InvalidOperator, secondNode,
                       "A range has one binder.", "", "Remove the second binder.");
            }
        } else {
            const ExpressionValue iterable = Check(context, iterableNode, NoType);
            if (iterable.kind != K::Error && RequireValue(context, iterable, iterableNode)) {
                const uint32 type = Materialize(context, iterable, iterableNode);
                const bool array = IsLanguage(type, "Array") && !IsOptional(type);
                const bool map = IsLanguage(type, "Map") && !IsOptional(type);
                const bool set = IsLanguage(type, "Set") && !IsOptional(type);
                if (IsOptional(type)) {
                    Report(context, DiagnosticCode::MayBeNull, iterableNode,
                           "The value may be null, so it cannot be iterated.", "",
                           "Use 'if (let x = value) { ... }' first.");
                } else if (array) {
                    firstType = two ? intType_ : Argument(type, 0);
                    secondType = Argument(type, 0);
                } else if (map) {
                    firstType = Argument(type, 0);
                    secondType = Argument(type, 1);
                } else if (set) {
                    firstType = Argument(type, 0);
                    if (two) {
                        Report(context, DiagnosticCode::InvalidOperator, secondNode,
                               "A set has one binder.", "", "Remove the second binder.");
                    }
                } else if (type != NoType) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.Append("A value of type '");
                    static_cast<void>(compilation_.AppendTypeText(message, type));
                    message.Append("' cannot be iterated.");
                    if (binder_.Succeeded(message)) {
                        Report(context, DiagnosticCode::InvalidOperator, iterableNode,
                               message.View(), "'for' goes over arrays, maps, sets and ranges.",
                               "Iterate a collection or a range 'a..<b'.");
                    }
                }
            }
        }

        if (first != NoSymbol) {
            c.symbols_[first].type = firstType;
        }

        if (second != NoSymbol) {
            c.symbols_[second].type = secondType;
        }

        CheckStatement(context, tree.Child(node, 4));
        break;
    }
    case SyntaxKind::While:
        condition(tree.Child(node, 0));
        CheckStatement(context, tree.Child(node, 1));
        break;
    case SyntaxKind::Switch:
        CheckSwitch(context, node);
        break;
    case SyntaxKind::Return:
        CheckReturn(context, node);
        break;
    case SyntaxKind::Emit: {
        // 이벤트 이름 하나(`emit Opened(...)`)면 인자를 그 매개변수 타입으로 본다(나머지는 LC3).
        const uint32 path = tree.Child(node, 0);
        const uint32 arguments = tree.Child(node, 1);
        uint32 signature = NoSignature;
        if (path != NoNode && tree.Node(path).childCount == 1 &&
            tree.Node(tree.Child(path, 0)).kind == SyntaxKind::Name) {
            const uint32 event = c.NodeSymbol(context.module, tree.Child(path, 0));
            if (event != NoSymbol && (c.symbols_[event].kind == SymbolKind::Event ||
                                      c.symbols_[event].kind == SymbolKind::EngineEvent)) {
                signature = c.symbols_[event].signature;
            }
        }

        const Span<const uint32> values = arguments != NoNode && tree.Node(arguments).kind == SyntaxKind::List
                                              ? tree.Children(arguments)
                                              : Span<const uint32>();
        if (signature != NoSignature && c.signatures_[signature].parameterCount != values.Size()) {
            Report(context, DiagnosticCode::ArgumentCount, node,
                   "The number of arguments does not match the event.", "",
                   "Pass one argument for each event parameter.");
            signature = NoSignature;
        }

        for (usize index = 0; index < values.Size(); ++index) {
            const uint32 parameter =
                signature == NoSignature
                    ? NoType
                    : c.signatureParameters_[c.signatures_[signature].firstParameter + index];
            const ExpressionValue value = Check(context, values[index], parameter);
            static_cast<void>(Coerce(context, value, parameter, values[index]));
        }

        break;
    }
    case SyntaxKind::Assignment: {
        const uint32 targetNode = tree.Child(node, 0);
        const uint32 valueNode = tree.Child(node, 1);
        const ExpressionValue target = Check(context, targetNode, NoType);
        uint32 type = NoType;
        if (target.kind == K::Typed) {
            // 원소 쓰기의 자리 타입은 원소 타입이다(읽기는 T?).
            type = tree.Node(targetNode).kind == SyntaxKind::Index ? NonOptional(target.type)
                                                                   : target.type;
        }

        const StringView op = binder_.NodeText(context.module, node);
        const ExpressionValue value = Check(context, valueNode, type);
        if (type == NoType) {
            break;
        }

        if (op == "=") {
            static_cast<void>(Coerce(context, value, type, valueNode));
            break;
        }

        // 복합 대입: `x op= v`는 `x = x op v`의 연산 규칙(3.10절).
        const NumericInfo numeric = Numeric(type);
        if (op == "/=" && (numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Unsigned)) {
            Report(context, DiagnosticCode::IntegerDivision, node,
                   "Integers are not divided with '/='.",
                   "Integer division drops the fraction, which is easy to miss.",
                   "Use 'x = Math.Div(x, y);'.");
            break;
        }

        if (numeric.kind != NumericKind::None) {
            static_cast<void>(Coerce(context, value, type, valueNode));
        } else if (IsVector(type)) {
            static_cast<void>(Coerce(context, value, op == "+=" || op == "-=" ? type : floatType_,
                                     valueNode));
        } else {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("'").Append(op).Append("' is not defined for '");
            static_cast<void>(compilation_.AppendTypeText(message, type));
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
                       IsLanguage(type, "string")
                           ? StringView("Strings are joined with interpolation, not '+'.")
                           : StringView(""),
                       IsLanguage(type, "string") ? StringView("Write 'text = \"${text}${more}\";'.")
                                                  : StringView("Use a number or a vector."));
            }
        }

        break;
    }
    case SyntaxKind::DiscardAssignment:
    case SyntaxKind::ExpressionStatement: {
        const uint32 valueNode = tree.Child(node, 0);
        const ExpressionValue value = Check(context, valueNode, NoType);
        if (value.kind == K::Wait || value.kind == K::Event) {
            Report(context, DiagnosticCode::Unawaited, valueNode,
                   "This must be awaited.", "A wait that is not awaited does nothing.",
                   "Write 'await' before it.");
        } else if (entry.kind == SyntaxKind::DiscardAssignment && value.kind != K::Void) {
            if (RequireValue(context, value, valueNode)) {
                static_cast<void>(Materialize(context, value, valueNode));
            }
        } else if (entry.kind == SyntaxKind::DiscardAssignment) {
            RequireValue(context, value, valueNode);
        } else if (value.kind == K::TypeName || value.kind == K::Namespace ||
                   value.kind == K::Function || value.kind == K::Literal) {
            RequireValue(context, value, valueNode);
        }

        break;
    }
    default:
        break;
    }
}

} // namespace rocks::sparks::detail
