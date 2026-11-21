#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "StringLiteralReader.h"
#include "TypeChecker.h"

// 선언 초기값과 본문의 문장이다(Docs/Design/SparksLanguage.ko.md 3.10절): 지역 선언의 타입,
// 조건(S0408), `for`, 대입의 타입, `switch`(S0415·S0416), 반환(S0409·S0410)과 "끝나는 문장".

namespace rocks::sparks::detail {

namespace {

// 필드·const 추론 상태다.
constexpr uint8 NotInferred = 0;
constexpr uint8 Inferring = 1;
constexpr uint8 Inferred = 2;

bool SameStringLiteral(StringView left, StringView right) noexcept {
    StringLiteralReader a(left);
    StringLiteralReader b(right);
    for (;;) {
        uint8 x = 0;
        uint8 y = 0;
        const bool moreA = a.Next(x);
        const bool moreB = b.Next(y);
        if (moreA != moreB) {
            return false;
        }

        if (!moreA) {
            return true;
        }

        if (x != y) {
            return false;
        }
    }
}

bool SameConstant(const ConstantValue& left, const ConstantValue& right) noexcept {
    if (left.kind != right.kind) {
        return false;
    }

    switch (left.kind) {
    case ConstantValue::Kind::Integer:
        return left.negative == right.negative && left.magnitude == right.magnitude;
    case ConstantValue::Kind::Bool:
        return left.boolean == right.boolean;
    case ConstantValue::Kind::String:
        return SameStringLiteral(left.text, right.text);
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
    flow_ = c.lastStage_ >= CompilationStage::Flow;
    rules_ = c.lastStage_ >= CompilationStage::Rules;
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

    // `void`·`void?`로 표기한 매개변수·필드는 해석하지 못한 이름이다(표기의 S0405는
    // CheckTypeRules).
    for (uint32 id = 0; id < c.symbols_.Size(); ++id) {
        const uint32 type = c.symbols_[id].type;
        if (type != NoType && NonOptional(type) == voidType_ &&
            (c.symbols_[id].kind == SymbolKind::Parameter ||
             c.symbols_[id].kind == SymbolKind::Field)) {
            c.symbols_[id].type = NoType;
        }
    }

    // enum case 목록(선언 순 — 심볼 번호 순)이다.
    if (auto resized = firstCase_.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    if (auto resized = nextCase_.TryResize(c.symbols_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < c.symbols_.Size(); ++id) {
        firstCase_[id] = NoSymbol;
        nextCase_[id] = NoSymbol;
    }

    for (uint32 id = static_cast<uint32>(c.symbols_.Size()); id > 0; --id) {
        const Symbol& symbol = c.symbols_[id - 1];
        if (symbol.kind == SymbolKind::EnumCase && symbol.owner != NoSymbol) {
            nextCase_[id - 1] = firstCase_[symbol.owner];
            firstCase_[symbol.owner] = id - 1;
        }
    }

    AnalyzeStructs(false);
    PrepareInitializers();
    CollectExtensionEvents(); // 확장 이벤트(3.12절): 식의 `X.E`가 모든 단계에서 찾는다

    for (uint32 module = 0; module < c.modules_.Size() && binder_.error_ == ErrorCode::None;
         ++module) {
        if (c.nodeBase_[module] != NoNode) {
            CheckModule(module);
        }
    }

    // 추론한 필드 타입까지 넣어 값 struct 순환을 알린다(S0426).
    if (binder_.error_ == ErrorCode::None) {
        AnalyzeStructs(true);
    }

    if (flow_ && binder_.error_ == ErrorCode::None) {
        CheckConstCycles(); // 3.11절 S0511
    }

    if (rules_ && binder_.error_ == ErrorCode::None) {
        CheckExtensionEvents(); // 3.12절 S0809·S0813·S0814
    }

    if (rules_ && binder_.error_ == ErrorCode::None) {
        AnalyzeEffects();      // 3.13절 S0601–S0603
        CheckResourceWaits();  // 3.14절 S0905(실제로 정지하는 await)
        CheckAcquireReturns(); // 3.14절 schema `acquire`
        CheckHeldEvents();     // 3.14절 schema 이벤트 인자
    }

    // typed IR(LC4a): 진단이 없는 컴파일만 내린다(타입·심볼이 모두 정해져 있다).
    if (c.lastStage_ >= CompilationStage::Codegen && binder_.error_ == ErrorCode::None &&
        binder_.diagnostics_.IsEmpty()) {
        LowerProgram();
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
    const bool annotated = tree.Child(entry.node, 3) != NoNode;
    if (initializer != NoNode && EnterInitializer(entry.module, initializer)) {
        // 표기를 해석하지 못한 필드는 기대 타입을 모르는 자리이며 초기값의 타입을 받지 않는다.
        const ExpressionValue value =
            Check(context, initializer, annotated && type == NoType ? UnresolvedType : type);
        if (type != NoType) {
            static_cast<void>(Coerce(context, value, type, initializer));
        } else if (!annotated) {
            type = Materialize(context, value, initializer);
        }

        --initializerDepth_;
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

    if (inference_[symbol] == Inferring) {
        ConstantValue cycle; // const 순환(진단은 LC2d) — 값 없이 더 알리지 않는다
        cycle.kind = ConstantValue::Kind::Invalid;
        return cycle;
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
    ConstantValue constant;
    constant.kind = ConstantValue::Kind::Invalid; // 초기값에 진단이 있으면 값 없이 조용히
    if (EnterInitializer(entry.module, initializer)) {
        const ExpressionValue value =
            Check(context, initializer, entry.type == NoType ? UnresolvedType : entry.type);
        if (entry.type != NoType && value.kind != ExpressionValue::Kind::Error &&
            Coerce(context, value, entry.type, initializer) != NoType &&
            value.constant.kind != ConstantValue::Kind::None) {
            constant = value.constant; // 상수식이 아닌 초기값(S0510)은 값 없이 Invalid
        }

        --initializerDepth_;
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
    if (flow_) {
        if (auto resized = staleRead_.TryResize(tree.NodeCount()); !resized) {
            binder_.error_ = resized.Error();
            return;
        }

        for (uint8& stale : staleRead_) {
            stale = 0;
        }

        staleModule_ = module;
    }

    CheckTypeRules(module);
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
            if (rules_) {
                CheckReentrancy(moduleContext, node, false, false, NoNode); // 3.12절 S0817
            }

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

            if (rules_) {
                CheckHandlers(typeContext, node); // 3.12절 hook·handler·이벤트 선언
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

    if (flow_ && binder_.error_ == ErrorCode::None) {
        CheckDeclarationRules(module); // 3.11절 필드·상수식·프로필
        if (rules_) {
            CheckAttributes(module); // 3.13절 S0620–S0623(attribute 인자의 const 사용을 표시한다)
            CheckResources(module);  // 3.14절 S0901–S0905
            CheckNetwork(module);    // 3.15절 S1001·S1002
        }

        CheckUnusedImports(module); // 3.11절 S0714
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
        // `void?`(S0405는 CheckTypeRules)는 해석하지 못한 반환 타입이다.
        if (context.returnType != NoType && context.returnType != voidType_ &&
            NonOptional(context.returnType) == voidType_) {
            context.returnType = NoType;
        }

        context.returnKnown = context.returnType != NoType;
        if (context.returnKnown && IsLanguage(context.returnType, "Result") &&
            !IsOptional(context.returnType)) {
            context.resultFunction = true;
            context.resultValue = Argument(context.returnType, 0);
        }
    }

    const uint32 block = tree.Child(function, handler ? 4 : 5);
    if (flow_) {
        AnalyzeFlow(context, function, true); // await 뒤 읽기(3.11절 — 본문 검사 전)
    }

    CheckStatement(context, block);
    if (flow_ && binder_.error_ == ErrorCode::None) {
        AnalyzeFlow(context, function, false); // S0710–S0712
    }

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
            static_cast<void>(Check(context, valueNode, UnresolvedType));
        }

        return;
    }

    const bool voidLike = context.returnType == voidType_;
    const bool resultVoid = context.resultFunction && context.resultValue == voidType_;
    if (valueNode == NoNode) {
        if (!voidLike && !resultVoid) {
            Report(context, DiagnosticCode::ReturnValue, node, "The function must return a value.",
                   "Its return type is not void.", "Write 'return value;'.");
        }

        return;
    }

    if (voidLike) {
        static_cast<void>(Check(context, valueNode, NoType));
        Report(context, DiagnosticCode::ReturnValue, valueNode, "This function returns no value.",
               "Its return type is void.", "Write 'return;'.");
        return;
    }

    if (context.resultFunction) {
        const ExpressionValue value =
            Check(context, valueNode, resultVoid ? NoType : context.resultValue);
        // `Error` 값은 실패다.
        if (value.kind == K::Typed && errorEnum_ != NoSymbol &&
            SymbolOf(value.type) == errorEnum_ && !IsOptional(value.type)) {
            return;
        }

        if (resultVoid) {
            if (value.kind != K::Error && RequireValue(context, value, valueNode)) {
                Report(context, DiagnosticCode::TypeMismatch, valueNode,
                       "A 'Result<void>' function returns no value on success.",
                       "It returns 'return;' for success or an 'Error' value for failure.",
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

uint32 TypeChecker::CheckSwitchValues(const CheckContext& context, uint32 node,
                                      Array<ConstantValue>& seen) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 subjectNode = tree.Child(node, 0);
    const ExpressionValue subject = Check(context, subjectNode, NoType);
    uint32 type = NoType;
    if (subject.kind != K::Error && RequireValue(context, subject, subjectNode)) {
        type = Materialize(context, subject, subjectNode);
        // 대상은 정수·bool·string·enum이다(case 값이 그 타입의 상수 — 3.10절 switch).
        const NumericInfo numeric = Numeric(type);
        const bool allowed =
            type != NoType && !IsOptional(type) &&
            (numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Unsigned ||
             type == boolType_ || type == stringType_ || IsEnumType(type));
        if (type != NoType && !allowed) {
            Report(context, DiagnosticCode::NotComparable, subjectNode,
                   "A switch needs an integer, 'bool', 'string' or enum value.",
                   "Cases are constants of the value's type.",
                   IsOptional(type) ? StringView("Unwrap it first with '?"
                                                 "?' or 'if (let)'.")
                                    : StringView("Use 'if' for this value."));
            type = NoType;
        }
    }

    const uint32 cases = tree.Child(node, 1);
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

                    if (Coerce(context, value, type, valueNode) == NoType ||
                        value.constant.kind == ConstantValue::Kind::Invalid) {
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
                        return NoType;
                    }
                }
            }
        }
    }

    return type;
}

void TypeChecker::CheckSwitch(const CheckContext& context, uint32 node) noexcept {
    // 본문 재귀의 frame은 작게 둔다: case 값은 CheckSwitchValues, 빠진 case는 CheckSwitchCoverage.
    const SyntaxTree& tree = compilation_.trees_[context.module];
    Array<ConstantValue> seen(allocator_);
    const uint32 type = CheckSwitchValues(context, node, seen);
    const uint32 cases = tree.Child(node, 1);
    if (cases != NoNode && tree.Node(cases).kind == SyntaxKind::List) {
        for (const uint32 item : tree.Children(cases)) {
            if (tree.Node(item).kind == SyntaxKind::SwitchCase) {
                CheckStatement(context, tree.Child(item, 1));
            }
        }
    }

    const uint32 fallback = tree.Child(node, 2);
    if (fallback != NoNode && tree.Node(fallback).kind == SyntaxKind::SwitchDefault) {
        CheckStatement(context, tree.Child(fallback, 0));
    }

    if (type != NoType && binder_.error_ == ErrorCode::None) {
        CheckSwitchCoverage(context, node, type, seen.View());
    }
}

void TypeChecker::CheckSwitchCoverage(const CheckContext& context, uint32 node, uint32 type,
                                      Span<const ConstantValue> seen) noexcept {
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 subjectNode = tree.Child(node, 0);
    const uint32 fallback = tree.Child(node, 2);

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
        for (uint32 at = firstCase_[subjectSymbol]; at != NoSymbol; at = nextCase_[at]) {
            bool covered = false;
            for (const ConstantValue& earlier : seen) {
                covered =
                    covered || (earlier.kind == ConstantValue::Kind::Enum && earlier.symbol == at);
            }

            if (!covered) {
                missing.Append(missingCount == 0 ? "" : ", ").Append(c.symbols_[at].name);
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
    if (node == NoNode || binder_.error_ != ErrorCode::None) {
        return;
    }

    // 문장은 블록·if·for·while·switch로 중첩된다. 재귀 경로의 frame을 작게 두려고 여기서는 본문만
    // 부르고 머리(조건·반복 대상·case 값)와 단순 문장은 따로 검사한다(CheckNode와 같은 이유).
    const SyntaxTree& tree = compilation_.trees_[context.module];
    switch (tree.Node(node).kind) {
    case SyntaxKind::Block:
        for (const uint32 statement : tree.Children(node)) {
            CheckStatement(context, statement);
        }

        break;
    case SyntaxKind::If:
        CheckIfHead(context, node);
        CheckStatement(context, tree.Child(node, 1));
        CheckStatement(context, tree.Child(node, 2));
        break;
    case SyntaxKind::For:
        CheckForHead(context, node);
        CheckStatement(context, tree.Child(node, 4));
        break;
    case SyntaxKind::While:
        CheckCondition(context, tree.Child(node, 0));
        CheckStatement(context, tree.Child(node, 1));
        break;
    case SyntaxKind::Switch:
        CheckSwitch(context, node);
        break;
    default:
        CheckSimpleStatement(context, node);
        break;
    }
}

void TypeChecker::CheckCondition(const CheckContext& context, uint32 expression) noexcept {
    using K = ExpressionValue::Kind;
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
}

void TypeChecker::CheckIfHead(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
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
        CheckCondition(context, test);
    }
}

void TypeChecker::CheckForHead(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    const uint32 iterableNode = tree.Child(node, 2);
    const uint32 endNode = tree.Child(node, 3);
    const uint32 first = c.NodeSymbol(context.module, tree.Child(node, 0));
    const uint32 secondNode = tree.Child(node, 1);
    const uint32 second =
        secondNode == NoNode ? NoSymbol : c.NodeSymbol(context.module, secondNode);
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
                Report(context, DiagnosticCode::InvalidOperator, iterableNode,
                       "A range goes over integers.", "", "Use integer bounds.");
            }
        } else if (Coerce(context, start, type, iterableNode) != NoType &&
                   Coerce(context, end, type, endNode) != NoType) {
            firstType = type;
        }

        if (two) {
            Report(context, DiagnosticCode::InvalidOperator, secondNode, "A range has one binder.",
                   "", "Remove the second binder.");
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
                AppendDisplayType(message, type);
                message.Append("' cannot be iterated.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::InvalidOperator, iterableNode, message.View(),
                           "'for' goes over arrays, maps, sets and ranges.",
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
}

void TypeChecker::CheckSimpleStatement(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    if (node == NoNode || binder_.error_ != ErrorCode::None) {
        return;
    }

    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::LocalVariable: {
        const uint32 nameNode = tree.Child(node, 0);
        const uint32 local = nameNode == NoNode ? NoSymbol : c.NodeSymbol(context.module, nameNode);
        const uint32 typeNode = tree.Child(node, 1);
        const uint32 declared = typeNode == NoNode ? NoType : c.NodeType(context.module, typeNode);
        const uint32 valueNode = tree.Child(node, 2);
        const uint32 exit = tree.Child(node, 3);
        const bool exits = exit != NoNode && tree.Node(exit).kind == SyntaxKind::ExitReturn;
        if (typeNode != NoNode && (declared == NoType || NonOptional(declared) == voidType_)) {
            // 표기를 해석하지 못했거나 `void` 표기(S0405는 CheckTypeRules)다: 값은 기대 타입을
            // 모르는 자리로 검사하고 이름은 해석하지 못한 것이다(초기값의 타입을 받지 않는다).
            static_cast<void>(Check(context, valueNode, UnresolvedType));
            if (exits) {
                CheckReturn(context, exit);
            }

            if (local != NoSymbol) {
                c.symbols_[local].type = NoType;
            }

            break;
        }

        const ExpressionValue value =
            Check(context, valueNode, exits ? MakeOptional(declared) : declared);
        uint32 type = NoType;
        if (exits) {
            if (value.kind != K::Error && !(value.kind == K::Typed && IsOptional(value.type))) {
                if (RequireValue(context, value, valueNode)) {
                    Report(context, DiagnosticCode::NotOptional, valueNode,
                           "'?? return' needs a value whose type has '?'.",
                           "It leaves the function when the value is null.", "Remove '?? return'.");
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
    case SyntaxKind::Return:
        CheckReturn(context, node);
        break;
    case SyntaxKind::Emit: {
        // 이벤트 이름 하나(`emit Opened(...)`)면 인자를 그 매개변수 타입으로 본다(나머지는 LC3).
        const uint32 path = tree.Child(node, 0);
        const uint32 arguments = tree.Child(node, 1);
        uint32 signature = NoSignature;
        uint32 event = NoSymbol;
        if (path != NoNode && tree.Node(path).childCount == 1 &&
            tree.Node(tree.Child(path, 0)).kind == SyntaxKind::Name) {
            event = c.NodeSymbol(context.module, tree.Child(path, 0));
        } else if (path != NoNode && tree.Node(path).childCount == 2 &&
                   tree.Node(tree.Child(path, 0)).kind == SyntaxKind::This &&
                   tree.Node(tree.Child(path, 1)).kind == SyntaxKind::Name &&
                   context.thisType != NoType) {
            // `emit this.이벤트`: class 기반 체인의 이벤트(3.10절 "emit").
            event = binder_.FindMemberInChain(
                SymbolOf(context.thisType), binder_.NodeText(context.module, tree.Child(path, 1)));
        }

        if (event != NoSymbol && (c.symbols_[event].kind == SymbolKind::Event ||
                                  c.symbols_[event].kind == SymbolKind::EngineEvent)) {
            signature = c.symbols_[event].signature;
        }

        if (rules_) {
            signature = ResolveEmit(context, node); // 3.12절: 발행할 수 있는 이벤트만
        }

        const Span<const uint32> values =
            arguments != NoNode && tree.Node(arguments).kind == SyntaxKind::List
                ? tree.Children(arguments)
                : Span<const uint32>();
        if (signature != NoSignature && c.signatures_[signature].parameterCount != values.Size()) {
            Report(context, DiagnosticCode::ArgumentCount, node,
                   "The number of arguments does not match the event.", "",
                   "Pass one argument for each event parameter.");
            signature = NoSignature;
        }

        for (usize index = 0; index < values.Size(); ++index) {
            uint32 parameter =
                signature == NoSignature
                    ? UnresolvedType
                    : c.signatureParameters_[c.signatures_[signature].firstParameter + index];
            parameter = parameter == NoType ? UnresolvedType : parameter;
            const ExpressionValue value = Check(context, values[index], parameter);
            static_cast<void>(Coerce(context, value, parameter, values[index]));
        }

        break;
    }
    case SyntaxKind::Assignment: {
        const uint32 targetNode = tree.Child(node, 0);
        const uint32 valueNode = tree.Child(node, 1);
        const ExpressionValue target = Check(context, targetNode, NoType);
        const StringView op = binder_.NodeText(context.module, node);
        uint32 type = NoType;     // 쓰기 자리의 타입(원소 쓰기는 T — 읽기는 T?)
        uint32 readType = NoType; // `x op= v`의 `x` 타입(읽기 타입)
        if (target.kind == K::Typed) {
            readType = target.type;
            type = tree.Node(targetNode).kind == SyntaxKind::Index ? NonOptional(target.type)
                                                                   : target.type;
            Record(context, targetNode, op == "=" ? type : readType);
        }

        const ExpressionValue value =
            Check(context, valueNode, target.kind == K::Typed ? type : UnresolvedType);
        const SyntaxKind targetKind = tree.Node(targetNode).kind;
        if (target.kind != K::Error &&
            (targetKind == SyntaxKind::Identifier || targetKind == SyntaxKind::Member ||
             targetKind == SyntaxKind::Index)) {
            CheckWritablePath(context, targetNode, targetNode); // 3.11절(S0211 대상은 제외)
        }

        if (target.kind != K::Typed) {
            break; // 대상에 진단이 있거나 값이 아닌 대상(S0706)이다
        }

        if (op == "=") {
            static_cast<void>(Coerce(context, value, type, valueNode));
            break;
        }

        CheckCompoundAssignment(context, node, op, readType, value, valueNode);
        break;
    }
    case SyntaxKind::DiscardAssignment:
    case SyntaxKind::ExpressionStatement: {
        const uint32 valueNode = tree.Child(node, 0);
        const ExpressionValue value = Check(context, valueNode, NoType);
        if (value.kind == K::Wait || value.kind == K::Event) {
            Report(context, DiagnosticCode::Unawaited, valueNode, "This must be awaited.",
                   "A wait that is not awaited does nothing.", "Write 'await' before it.");
        } else if (entry.kind == SyntaxKind::DiscardAssignment && value.kind != K::Void) {
            if (RequireValue(context, value, valueNode)) {
                static_cast<void>(Materialize(context, value, valueNode));
            }
        } else if (entry.kind == SyntaxKind::DiscardAssignment) {
            RequireValue(context, value, valueNode);
        } else if (value.kind == K::TypeName || value.kind == K::Namespace ||
                   value.kind == K::Function || value.kind == K::Literal) {
            RequireValue(context, value, valueNode);
        } else if (flow_ &&
                   (value.kind == K::Typed || value.kind == K::UntypedInt ||
                    value.kind == K::UntypedFloat || value.kind == K::Null) &&
                   !(rules_ && value.kind == K::Typed && IsHeldType(value.type))) {
            // 버린 유지 자원은 S0901이 알린다(3.14절).
            // 버린 결과(3.11절, S0713).
            Report(context, DiagnosticCode::DiscardedResult, valueNode,
                   "The result of this expression is not used.",
                   "A value that is computed and dropped is usually a mistake.",
                   "Use the value, or write '_ = ...' to discard it on purpose.");
        }

        break;
    }
    default:
        break;
    }
}

void TypeChecker::CheckCompoundAssignment(const CheckContext& context, uint32 node, StringView op,
                                          uint32 readType, const ExpressionValue& value,
                                          uint32 valueNode) noexcept {
    using K = ExpressionValue::Kind;
    if (value.kind == K::Error) {
        return;
    }

    // S0406: 그 타입·연산에 없는 복합 대입이다(valueType을 모르면 대상 타입만 적는다).
    auto invalid = [&](uint32 valueType) noexcept {
        const bool strings = IsLanguage(readType, "string");
        const bool optional = IsOptional(readType);
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("'").Append(op).Append("' is not defined for '");
        AppendDisplayType(message, readType);
        if (valueType != NoType) {
            message.Append("' and '");
            AppendDisplayType(message, valueType);
        }

        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
                   strings    ? StringView("Strings are joined with interpolation, not '+'.")
                   : optional ? StringView("A value whose type has '?' may be null.")
                              : StringView(""),
                   strings    ? StringView("Write 'text = \"${text}${more}\";'.")
                   : optional ? StringView("Write 'x = (x ?"
                                           "? 0) + value;' or unwrap it with 'if (let)'.")
                              : StringView("Use a number or a vector."));
        }
    };
    const bool untyped = value.kind == K::UntypedInt || value.kind == K::UntypedFloat;
    // 값의 타입(타입 없는 상수는 기본 타입으로 기록)이다. 값이 아니면 진단을 내고 NoType.
    auto valueType = [&]() noexcept {
        if (value.kind == K::Typed) {
            return value.type;
        }

        return untyped ? Materialize(context, value, valueNode) : NoType;
    };
    const NumericInfo numeric = Numeric(readType);
    if (op == "/=" &&
        (numeric.kind == NumericKind::Signed || numeric.kind == NumericKind::Unsigned)) {
        Report(context, DiagnosticCode::IntegerDivision, node,
               "Integers are not divided with '/='.",
               "Integer division drops the fraction, which is easy to miss.",
               "Use 'x = Math.Div(x, y);'.");
        return;
    }

    if (numeric.kind != NumericKind::None) {
        if (value.kind != K::Typed) {
            static_cast<void>(Coerce(context, value, readType, valueNode)); // 상수·null
            return;
        }

        if (value.type == readType) {
            return;
        }

        if (Numeric(value.type).kind != NumericKind::None) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("'").Append(op).Append("' mixes '");
            AppendDisplayType(message, readType);
            message.Append("' and '");
            AppendDisplayType(message, value.type);
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::MixedNumericTypes, node, message.View(),
                       "Numbers of different types are never mixed silently.",
                       "Convert the value, as in 'float(x)'.");
            }

            return;
        }

        invalid(value.type);
        return;
    }

    if (!RequireValue(context, value, valueNode)) {
        return;
    }

    if (IsVector(readType)) {
        const bool additive = op == "+=" || op == "-=";
        if (!additive && value.kind != K::Typed) {
            static_cast<void>(Coerce(context, value, floatType_, valueNode)); // 상수·null → float
            return;
        }

        const uint32 type = valueType();
        if (type != NoType && type != (additive ? readType : floatType_)) {
            invalid(type);
        }

        return;
    }

    const bool composable = op == "*=" && !IsOptional(readType) &&
                            (IsLanguage(readType, "quat") || IsLanguage(readType, "mat4"));
    const uint32 type = value.kind == K::Null ? NoType : valueType();
    if (composable && type == readType) {
        return;
    }

    if (value.kind == K::Null || type != NoType) {
        invalid(type);
    }
}

} // namespace rocks::sparks::detail
