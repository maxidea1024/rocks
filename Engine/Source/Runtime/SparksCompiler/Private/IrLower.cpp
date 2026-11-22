#include <Base/Checked.h>
#include <Core/Text/StringBuilder.h>

#include "CompilationChecker.h"
#include "IrBuilder.h"
#include "LanguageNames.h"
#include <SparksCompiler/Literal.h>

// 식·문장 → typed IR 내림이다(Docs/Design/SparksBytecode.ko.md 3절, LC4a). 값은 slot 묶음으로,
// `&&`·`||`·`??`·`?.`·`if (let)`·반복·switch는 블록과 분기로 내린다.

namespace rocks::sparks::detail {

bool IrBuilder::Build() noexcept {
    const SyntaxNode& entry = tree_->Node(function_.node);
    if (entry.kind == SyntaxKind::Class) {
        return BuildInitializer();
    }

    const bool handler = entry.kind == SyntaxKind::Handler;
    const uint32 block = tree_->Child(function_.node, handler ? 4 : 5);
    if (function_.symbol != NoSymbol) {
        const uint32 signature = c_.symbols_[function_.symbol].signature;
        returnType_ = signature == NoSignature ? 0 : c_.signatures_[signature].returnType;
    }

    // this(class·struct 본문)와 매개변수 묶음.
    const uint32 owner = function_.owner;
    if (owner != NoSymbol && (c_.symbols_[owner].kind == SymbolKind::Class ||
                              c_.symbols_[owner].kind == SymbolKind::Struct)) {
        this_ = NewValue(checker_.TypeOfSymbol(owner), "this");
        function_.self = this_;
    }

    const uint32 parameters = tree_->Child(function_.node, 3);
    if (parameters != NoNode && tree_->Node(parameters).kind == SyntaxKind::List) {
        for (const uint32 parameter : tree_->Children(parameters)) {
            if (tree_->Node(parameter).kind != SyntaxKind::Parameter) {
                continue;
            }

            const uint32 nameNode = tree_->Child(parameter, 0);
            const uint32 type = c_.NodeType(module_, tree_->Child(parameter, 1));
            const bool named = nameNode != NoNode && tree_->Node(nameNode).kind == SyntaxKind::Name;
            const uint32 first = NewValue(type, named ? checker_.binder_.NodeText(module_, nameNode)
                                                      : StringView("_"));
            if (auto pushed = function_.parameters.TryPushBack(first); !pushed) {
                Fail(pushed.Error());
                return false;
            }

            const uint32 symbol = named ? c_.NodeSymbol(module_, nameNode) : NoSymbol;
            if (symbol != NoSymbol) {
                BindLocal(symbol, first);
            }
        }
    }

    StartBlock(NewBlock());
    LowerStatement(block);
    if (!Terminated()) {
        // 끝에 도달하는 경로: void(또는 Result<void>)는 반환, 그 밖은 도달하지 않는다(S0409가
        // 막았다). 위치는 본문의 닫는 `}`다.
        LowerReturn(NoNode, NoNode);
        if (!function_.instructions.IsEmpty() && block != NoNode) {
            const SourceSpan body = tree_->Node(block).span;
            const SourceSpan closing{body.end > body.begin ? body.end - 1 : body.begin, body.end};
            for (usize index = function_.instructions.Size(); index > 0; --index) {
                IrInstruction& instruction = function_.instructions[index - 1];
                if (instruction.span.begin != 0 || instruction.span.end != 0) {
                    break;
                }

                instruction.span = closing;
            }
        }
    }

    return !Failed();
}

bool IrBuilder::BuildInitializer() noexcept {
    // this(class)와 자기 필드의 초기값(SparksRuntime 2.1절 — 기반 클래스의 필드는 기반의 $init이
    // 쓴다). 위치는 필드 선언이다.
    const uint32 owner = function_.owner;
    this_ = NewValue(checker_.TypeOfSymbol(owner), "this");
    function_.self = this_;
    StartBlock(NewBlock());
    const uint32 members = tree_->Child(function_.node, 4);
    for (const uint32 member :
         members == NoNode ? Span<const uint32>() : tree_->Children(members)) {
        if (tree_->Node(member).kind != SyntaxKind::Field) {
            continue;
        }

        const uint32 field = c_.NodeSymbol(module_, tree_->Child(member, 2));
        if (field == NoSymbol) {
            continue;
        }

        const uint32 value = LowerFieldDefault(field, member);
        if (value != NoIrRegister) {
            Add(IrOp::SetField, this_, value, NoIrRegister, field, member);
        }
    }

    LowerReturn(NoNode, NoNode);
    // 반환의 위치는 class 본문의 닫는 `}`다.
    const SourceSpan body = tree_->Node(function_.node).span;
    const SourceSpan closing{body.end > body.begin ? body.end - 1 : body.begin, body.end};
    for (usize index = function_.instructions.Size(); index > 0; --index) {
        IrInstruction& instruction = function_.instructions[index - 1];
        if (instruction.span.begin != 0 || instruction.span.end != 0) {
            break;
        }

        instruction.span = closing;
    }

    return !Failed();
}

// ---- 상수 ----

bool IrBuilder::EvaluateConstant(uint32 node, ConstantValue& value) noexcept {
    if (node == NoNode) {
        return false;
    }

    const SyntaxNode& entry = tree_->Node(node);
    switch (entry.kind) {
    case SyntaxKind::IntegerLiteral: {
        auto decoded = TryDecodeIntegerLiteral(checker_.TokenText(module_, node));
        if (!decoded) {
            return false;
        }

        value = ConstantValue{};
        value.kind = ConstantValue::Kind::Integer;
        value.magnitude = decoded.Value();
        return true;
    }
    case SyntaxKind::FloatLiteral: {
        auto decoded =
            TryDecodeFloatLiteral(checker_.allocator_, checker_.TokenText(module_, node));
        if (!decoded) {
            return false;
        }

        value = ConstantValue{};
        value.kind = ConstantValue::Kind::Float;
        value.real = decoded.Value();
        return true;
    }
    case SyntaxKind::BoolLiteral:
        value = ConstantValue{};
        value.kind = ConstantValue::Kind::Bool;
        value.boolean = checker_.TokenText(module_, node) == "true";
        return true;
    case SyntaxKind::Parenthesized:
        return EvaluateConstant(tree_->Child(node, 0), value);
    case SyntaxKind::Identifier:
    case SyntaxKind::Member: {
        const uint32 symbol = c_.NodeSymbol(module_, node);
        if (symbol == NoSymbol || c_.symbols_[symbol].kind != SymbolKind::Const) {
            return false;
        }

        value = checker_.ConstOf(symbol);
        return value.kind == ConstantValue::Kind::Integer ||
               value.kind == ConstantValue::Kind::Float || value.kind == ConstantValue::Kind::Bool;
    }
    case SyntaxKind::Unary: {
        if (!EvaluateConstant(tree_->Child(node, 0), value)) {
            return false;
        }

        const StringView op = checker_.TokenText(module_, node);
        if (op == "-" && value.kind == ConstantValue::Kind::Integer) {
            value.negative = !value.negative && value.magnitude != 0;
            return true;
        }

        if (op == "-" && value.kind == ConstantValue::Kind::Float) {
            value.real = -value.real;
            return true;
        }

        if (op == "!" && value.kind == ConstantValue::Kind::Bool) {
            value.boolean = !value.boolean;
            return true;
        }

        return false;
    }
    case SyntaxKind::Binary:
        return EvaluateBinaryConstant(node, value);
    default:
        return false;
    }
}

bool IrBuilder::EvaluateBinaryConstant(uint32 node, ConstantValue& value) noexcept {
    // 정해진 상수의 산술(SD66 — 검사기가 같은 값을 범위 검사했다). 정수 overflow는 접지 않는다(실행
    // 중 계산으로 남긴다 — 검사기가 이미 S0503을 냈어야 하는 자리다).
    ConstantValue left;
    ConstantValue right;
    const StringView op = checker_.TokenText(module_, node);
    if ((op != "+" && op != "-" && op != "*" && op != "/" && op != "%") ||
        !EvaluateConstant(tree_->Child(node, 1), right) || // 오른쪽 먼저: 왼쪽으로 깊은
        !EvaluateConstant(tree_->Child(node, 0), left)) {  // 변수 사슬은 바로 끝난다
        return false;
    }

    const bool integers =
        left.kind == ConstantValue::Kind::Integer && right.kind == ConstantValue::Kind::Integer;
    if (!integers &&
        (left.kind == ConstantValue::Kind::Bool || right.kind == ConstantValue::Kind::Bool)) {
        return false;
    }

    value = ConstantValue{};
    if (integers) {
        const int64 a = left.negative ? int64(0) - int64(left.magnitude) : int64(left.magnitude);
        const int64 b = right.negative ? int64(0) - int64(right.magnitude) : int64(right.magnitude);
        if ((left.magnitude >> 63) != 0 || (right.magnitude >> 63) != 0) {
            return false; // int64 밖(uint64 상수) — 실행 중 계산으로 남긴다
        }

        int64 result = 0;
        bool overflow = false;
        if (op == "+") {
            overflow = !TryAdd(a, b, result);
        } else if (op == "-") {
            overflow = !TrySubtract(a, b, result);
        } else if (op == "*") {
            overflow = !TryMultiply(a, b, result);
        } else if (b == 0 || (a == NumericLimits<int64>::Min() && b == -1)) {
            return false;
        } else if (op == "%") {
            // 내림 방식(SL 3.3절 — 검사기의 RemainderConstant·VM과 같다).
            result = a % b;
            if (result != 0 && ((result < 0) != (b < 0))) {
                result += b;
            }
        } else if (a % b == 0) {
            result = a / b;
        } else {
            // 나누어떨어지지 않는 정해진 상수의 `/`는 실수다(실수 자리에서만 검사를 통과한다).
            value.kind = ConstantValue::Kind::Float;
            value.real = static_cast<double>(a) / static_cast<double>(b);
            return true;
        }

        if (overflow) {
            return false;
        }

        value.kind = ConstantValue::Kind::Integer;
        value.negative = result < 0;
        value.magnitude = result < 0 ? uint64(0) - uint64(result) : uint64(result);
        return true;
    }

    auto real = [](const ConstantValue& v) noexcept {
        if (v.kind == ConstantValue::Kind::Float) {
            return v.real;
        }

        const double magnitude = static_cast<double>(v.magnitude);
        return v.negative ? -magnitude : magnitude;
    };
    if (op == "%") {
        return false; // `%`는 정수 전용이다
    }

    const double a = real(left);
    const double b = real(right);
    value.kind = ConstantValue::Kind::Float;
    value.real = op == "+" ? a + b : op == "-" ? a - b : op == "*" ? a * b : a / b;
    return value.real - value.real == 0.0; // 유한한 값만
}

uint32 IrBuilder::LoadConstant(const ConstantValue& value, uint32 type, uint32 node) noexcept {
    const uint32 first = NewValue(type);
    if (first == NoIrRegister) {
        return NoIrRegister;
    }

    const IrRegister& slot = function_.registers[first];
    uint32 target = first;
    if (slot.scalar == IrScalar::Flag && SlotCount(first) > 1) {
        Add(IrOp::LoadInt, first, 1, NoIrRegister, 0, node); // T?의 있음 표지
        target = first + 1;
    }

    const IrScalar scalar = function_.registers[target].scalar;
    uint64 bits = 0;
    if (scalar == IrScalar::F32 || scalar == IrScalar::F64) {
        double real = value.real;
        if (value.kind == ConstantValue::Kind::Integer) {
            const double magnitude = static_cast<double>(value.magnitude);
            real = value.negative ? -magnitude : magnitude;
        }

        if (scalar == IrScalar::F32) {
            real = static_cast<double>(
                static_cast<float>(real)); // float32 값은 float로 반올림한 double
        }

        bits = FloatBits(real);
        Add(IrOp::LoadFloat, target, static_cast<uint32>(bits), NoIrRegister,
            static_cast<uint32>(bits >> 32), node);
        return first;
    }

    if (value.kind == ConstantValue::Kind::Bool) {
        bits = value.boolean ? 1 : 0;
    } else if (value.kind == ConstantValue::Kind::Enum) {
        bits = EnumIndex(value.symbol);
    } else {
        bits = value.negative ? uint64(0) - value.magnitude : value.magnitude;
    }

    Add(IrOp::LoadInt, target, static_cast<uint32>(bits), NoIrRegister,
        static_cast<uint32>(bits >> 32), node);
    return first;
}

uint64 IrBuilder::EnumIndex(uint32 caseSymbol) const noexcept {
    const Symbol& entry = c_.symbols_[caseSymbol];
    if (entry.owner != NoSymbol && c_.symbols_[entry.owner].kind == SymbolKind::EngineType) {
        return entry.item;
    }

    uint64 index = 0;
    for (uint32 at = entry.owner < checker_.firstCase_.Size() ? checker_.firstCase_[entry.owner]
                                                              : NoSymbol;
         at != NoSymbol && at != caseSymbol; at = checker_.nextCase_[at]) {
        ++index;
    }

    return index;
}

// ---- 식 ----

uint32 IrBuilder::TypeOf(uint32 node, uint32 expected) const noexcept {
    const uint32 type = c_.ExpressionType(module_, node);
    return type != NoType ? type : expected;
}

uint32 IrBuilder::Lower(uint32 node, uint32 expected) noexcept {
    if (node == NoNode || Failed()) {
        return NoIrRegister;
    }

    const SyntaxNode& entry = tree_->Node(node);
    // 정해진 상수식(`300 - 100`, `-1`, 타입 없는 상수)은 검사기처럼 한 번에 접어 그 식의
    // 타입(없으면 자리의 타입)으로 싣는다(SD66 — 연산마다 내리면 중간 값이 자리 타입의 범위를 넘을
    // 수 있다).
    const bool folding =
        entry.kind == SyntaxKind::Binary || entry.kind == SyntaxKind::Unary ||
        (c_.ExpressionType(module_, node) == NoType && entry.kind != SyntaxKind::NullLiteral &&
         entry.kind != SyntaxKind::CollectionLiteral);
    if (folding) {
        ConstantValue value;
        if (EvaluateConstant(node, value)) {
            uint32 type = TypeOf(node, expected);
            if (type == NoType) {
                type = value.kind == ConstantValue::Kind::Float  ? checker_.floatType_
                       : value.kind == ConstantValue::Kind::Bool ? checker_.boolType_
                                                                 : checker_.intType_;
            }

            return LoadConstant(value, type, node);
        }
    }

    switch (entry.kind) {
    case SyntaxKind::IntegerLiteral:
    case SyntaxKind::FloatLiteral:
    case SyntaxKind::BoolLiteral: {
        ConstantValue value;
        return EvaluateConstant(node, value) ? LoadConstant(value, TypeOf(node, expected), node)
                                             : NoIrRegister;
    }
    case SyntaxKind::NullLiteral: {
        const uint32 first = NewValue(expected);
        LoadNone(first, node);
        return first;
    }
    case SyntaxKind::Parenthesized:
        return Lower(tree_->Child(node, 0), expected);
    case SyntaxKind::This:
        return this_;
    case SyntaxKind::Identifier:
        return LowerIdentifier(node);
    case SyntaxKind::Binary:
        return LowerBinary(node, expected);
    case SyntaxKind::Unary:
        return LowerUnary(node, expected);
    case SyntaxKind::Member:
        return LowerMember(node);
    case SyntaxKind::Call:
        return LowerCall(node, expected);
    case SyntaxKind::Index:
        return LowerIndex(node);
    case SyntaxKind::String:
    case SyntaxKind::VerbatimString:
        return LowerString(node);
    case SyntaxKind::CollectionLiteral:
        return LowerCollection(node, expected);
    case SyntaxKind::CastTest:
        return LowerCast(node);
    case SyntaxKind::Await:
        return LowerAwait(node, tree_->Child(node, 0));
    case SyntaxKind::Try:
        return LowerTry(node);
    default:
        return NoIrRegister;
    }
}

uint32 IrBuilder::Coerce(uint32 value, uint32 to, uint32 node) noexcept {
    // 값 묶음을 자리의 타입으로: 같으면 그대로, T → T?는 있음 표지를 더한다.
    if (value == NoIrRegister || to == NoType) {
        return value;
    }

    const uint32 from = function_.registers[value].type;
    if (from == to) {
        return value;
    }

    const IrLayout target = Layout(checker_, to);
    if (target.slots == SlotCount(value) && target.bank == function_.registers[value].bank) {
        // 같은 배치(파생 → 기반, Ref·Buf의 T → T?): 타입만 다른 묶음으로 옮긴다.
        const uint32 result = NewValue(to);
        MoveValue(result, value, node);
        return result;
    }

    if (target.bank == IrBank::Scalar && target.slots == SlotCount(value) + 1 &&
        target.scalars[0] == IrScalar::Flag) {
        const uint32 result = NewValue(to);
        Add(IrOp::LoadInt, result, 1, NoIrRegister, 0, node);
        for (uint32 slot = 0; slot < SlotCount(value); ++slot) {
            Add(IrOp::Move, result + 1 + slot, value + slot, NoIrRegister, 0, node);
        }

        return result;
    }

    return value;
}

uint32 IrBuilder::LowerIdentifier(uint32 node) noexcept {
    const uint32 symbol = c_.NodeSymbol(module_, node);
    if (symbol == NoSymbol) {
        return NoIrRegister;
    }

    const Symbol& entry = c_.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::Local:
    case SymbolKind::Parameter: {
        const uint32 local = LocalRegister(symbol);
        const uint32 type = c_.ExpressionType(module_, node);
        // await 뒤 읽기의 `T?`(3.11절)는 같은 Ref 표현이다.
        return type != NoType && type != function_.registers[local].type ? Coerce(local, type, node)
                                                                         : local;
    }
    case SymbolKind::Field:
    case SymbolKind::EngineField:
    case SymbolKind::ContextName:
    case SymbolKind::LanguageField: {
        const uint32 result = NewValue(TypeOf(node, entry.type));
        Add(IrOp::GetField, result, entry.kind == SymbolKind::ContextName ? NoIrRegister : this_,
            NoIrRegister, symbol, node);
        return result;
    }
    case SymbolKind::Const:
        return LowerConst(symbol, TypeOf(node, entry.type), node);
    default:
        return NoIrRegister; // 타입 이름·이름 공간·함수 값(호출이 본다)
    }
}

uint32 IrBuilder::LowerConst(uint32 symbol, uint32 type, uint32 node) noexcept {
    // 접힌 값(숫자·bool·문자열·enum)은 상수로, 그 밖(수학 값·Tag·`Math.Radians(...)` 등 접지 않는
    // 정해진 식)은 초기값 식을 이 자리에서 내린다.
    const ConstantValue value = checker_.ConstOf(symbol);
    if (value.kind == ConstantValue::Kind::Integer || value.kind == ConstantValue::Kind::Float ||
        value.kind == ConstantValue::Kind::Bool || value.kind == ConstantValue::Kind::String ||
        value.kind == ConstantValue::Kind::Enum) {
        return LoadConstantOrString(value, type, node);
    }

    return LowerConstInitializer(symbol, node);
}

uint32 IrBuilder::LowerConstInitializer(uint32 constSymbol, uint32 useNode) noexcept {
    const Symbol& entry = c_.symbols_[constSymbol];
    const SyntaxTree& tree = c_.trees_[entry.module];
    const uint32 initializer = tree.Child(entry.node, 4);
    const uint32 type = entry.type;
    // 초기값은 const를 선언한 모듈의 식이다: 그동안 모듈·트리를 바꾸고, 명령 위치는 쓴 자리로 둔다.
    const SyntaxTree* savedTree = tree_;
    const uint32 savedModule = module_;
    const bool savedOverride = overrideSpan_;
    const SourceSpan savedSpan = spanOverride_;
    if (!overrideSpan_) {
        spanOverride_ = tree_->Node(useNode).span;
        overrideSpan_ = true;
    }

    tree_ = &tree;
    module_ = entry.module;
    const uint32 value = Coerce(Lower(initializer, type), type, initializer);
    tree_ = savedTree;
    module_ = savedModule;
    overrideSpan_ = savedOverride;
    spanOverride_ = savedSpan;
    return value;
}

uint32 IrBuilder::LoadConstantOrString(const ConstantValue& value, uint32 type,
                                       uint32 node) noexcept {
    if (value.kind == ConstantValue::Kind::String) {
        const uint32 result = NewValue(type);
        Add(IrOp::LoadString, result, AddString(value.text), NoIrRegister, 0, node);
        return result;
    }

    return LoadConstant(value, type, node);
}

uint32 IrBuilder::AddString(StringView text) noexcept {
    for (usize index = 0; index < function_.strings.Size(); ++index) {
        if (function_.strings[index] == text) {
            return static_cast<uint32>(index);
        }
    }

    if (auto pushed = function_.strings.TryPushBack(text); !pushed) {
        Fail(pushed.Error());
        return 0;
    }

    return static_cast<uint32>(function_.strings.Size() - 1);
}

uint32 IrBuilder::LowerBinary(uint32 node, uint32 expected) noexcept {
    const StringView op = checker_.TokenText(module_, node);
    if (op == "&&" || op == "||") {
        return LowerLogical(node);
    }

    if (op == "??") {
        return LowerCoalesce(node, expected);
    }

    // 왼쪽으로 깊은 사슬(`x + x + ...` — 트리 깊이 상한까지)은 재귀하지 않는다: 타입이 정해진 산술·
    // 비교 노드의 왼쪽 척추를 모아 가장 안쪽부터 차례로 내린다(오른쪽 피연산자만 재귀).
    Array<uint32> spine(checker_.allocator_);
    uint32 leaf = node;
    while (leaf != NoNode && tree_->Node(leaf).kind == SyntaxKind::Binary &&
           (leaf == node || c_.ExpressionType(module_, leaf) != NoType)) {
        const StringView step = checker_.TokenText(module_, leaf);
        if (step == "&&" || step == "||" || step == "??") {
            break;
        }

        if (auto pushed = spine.TryPushBack(leaf); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }

        leaf = tree_->Child(leaf, 0);
    }

    uint32 value = NoIrRegister;
    for (usize index = spine.Size(); index > 0; --index) {
        const uint32 current = spine[index - 1];
        value = LowerBinaryStep(current, index == spine.Size() ? NoIrRegister : value,
                                index == 1 ? expected : NoType);
    }

    return value;
}

uint32 IrBuilder::LowerBinaryStep(uint32 node, uint32 left, uint32 expected) noexcept {
    // left가 NoIrRegister면 왼쪽 피연산자를 여기서 내린다(척추의 가장 안쪽).
    const StringView op = checker_.TokenText(module_, node);
    const uint32 resultType = TypeOf(node, expected);
    const uint32 leftNode = tree_->Child(node, 0);
    const uint32 rightNode = tree_->Child(node, 1);
    const bool comparison =
        op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    // 피연산자의 타입: 비교는 두 쪽 중 타입이 있는 쪽, 산술은 결과 타입(벡터 × 실수는 각자).
    uint32 leftType = c_.ExpressionType(module_, leftNode);
    uint32 rightType = c_.ExpressionType(module_, rightNode);
    if (comparison) {
        leftType = leftType == NoType ? rightType : leftType;
        rightType = rightType == NoType ? leftType : rightType;
    } else {
        leftType = leftType == NoType ? resultType : leftType;
        rightType = rightType == NoType ? resultType : rightType;
    }

    if (left == NoIrRegister) {
        left = Lower(leftNode, leftType);
    }

    uint32 right = Lower(rightNode, rightType);
    // `T? == T`: optional이 아닌 쪽을 `T?`로 올려 묶음 크기를 맞춘다(Scalar `T?`는 slot이 하나 더
    // 많다).
    if (comparison && left != NoIrRegister && right != NoIrRegister &&
        SlotCount(left) != SlotCount(right)) {
        if (SlotCount(left) < SlotCount(right)) {
            left = Coerce(left, function_.registers[right].type, leftNode);
        } else {
            right = Coerce(right, function_.registers[left].type, rightNode);
        }
    }

    const uint32 result = NewValue(resultType);
    if (left == NoIrRegister || right == NoIrRegister || result == NoIrRegister) {
        return result;
    }

    if (comparison) {
        const IrCompare kind = op == "!="   ? IrCompare::NotEqual
                               : op == "<"  ? IrCompare::Less
                               : op == "<=" ? IrCompare::LessEqual
                               : op == ">"  ? IrCompare::Greater
                               : op == ">=" ? IrCompare::GreaterEqual
                                            : IrCompare::Equal;
        // 묶음 비교(slot 수 extra). null과의 비교는 있음 표지·handle로 한다.
        Add(IrOp::Compare, result, left, right, SlotCount(left), node, static_cast<uint8>(kind));
        return result;
    }

    // quat·mat4의 `*`는 성분별이 아니라 합성이다(UE FQuat·FMatrix 곱) — 묶음 명령 하나.
    if (op == "*" &&
        (checker_.IsLanguage(resultType, "quat") || checker_.IsLanguage(resultType, "mat4"))) {
        Add(IrOp::Binary, result, left, right, 0, node, static_cast<uint8>(IrArith::Compose));
        return result;
    }

    // 산술은 Scalar 값뿐이다(문자열 `+`는 없다 — 보간을 쓴다, SL §3.2).
    const IrArith kind = op == "+"   ? IrArith::Add
                         : op == "-" ? IrArith::Sub
                         : op == "*" ? IrArith::Mul
                         : op == "/" ? IrArith::Div
                                     : IrArith::Mod;
    // 성분별 연산(벡터 ± 벡터, 벡터 × 실수 — 실수 쪽은 같은 slot을 되풀이한다).
    const uint32 count = SlotCount(result);
    for (uint32 slot = 0; slot < count; ++slot) {
        const uint32 a = left + (SlotCount(left) == 1 ? 0 : slot);
        const uint32 b = right + (SlotCount(right) == 1 ? 0 : slot);
        Add(IrOp::Binary, result + slot, a, b, 0, node, static_cast<uint8>(kind));
    }

    return result;
}

uint32 IrBuilder::LowerLogical(uint32 node) noexcept {
    // a && b && c: 앞에서부터 하나라도 거짓이면 false, 아니면 마지막 값. `||`는 참이면 true. 같은
    // 연산자의 왼쪽 척추는 모아 반복으로 내린다(깊은 사슬에서 재귀하지 않는다).
    const StringView op = checker_.TokenText(module_, node);
    const bool andOperator = op == "&&";
    Array<uint32> terms(checker_.allocator_);
    uint32 leaf = node;
    while (leaf != NoNode && tree_->Node(leaf).kind == SyntaxKind::Binary &&
           checker_.TokenText(module_, leaf) == op) {
        if (auto pushed = terms.TryPushBack(tree_->Child(leaf, 1)); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }

        leaf = tree_->Child(leaf, 0);
    }

    if (auto pushed = terms.TryPushBack(leaf); !pushed) {
        Fail(pushed.Error());
        return NoIrRegister;
    }

    // terms는 오른쪽 항부터 담겼다 — 뒤에서부터가 원문 순서다.
    const uint32 result = NewValue(checker_.boolType_);
    const uint32 shortBlock = NewBlock();
    const uint32 done = NewBlock();
    for (usize index = terms.Size(); index > 1; --index) {
        const uint32 next = NewBlock();
        LowerCondition(terms[index - 1], andOperator ? next : shortBlock,
                       andOperator ? shortBlock : next);
        StartBlock(next);
    }

    MoveValue(result, Lower(terms[0], checker_.boolType_), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(shortBlock);
    Add(IrOp::LoadInt, result, andOperator ? 0 : 1, NoIrRegister, 0, node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(done);
    return result;
}

void IrBuilder::LowerCondition(uint32 node, uint32 onTrue, uint32 onFalse) noexcept {
    // 분기 자리의 조건은 값을 만들지 않고 단락 분기로 내린다: `!`는 갈래를 바꾸고, `&&`·`||`는 같은
    // 연산자의 왼쪽 척추를 모아 항마다 분기한다(깊은 사슬에서 재귀하지 않는다).
    while (node != NoNode && !Failed()) {
        const SyntaxNode& entry = tree_->Node(node);
        if (entry.kind == SyntaxKind::Parenthesized) {
            node = tree_->Child(node, 0);
            continue;
        }

        if (entry.kind == SyntaxKind::Unary && checker_.TokenText(module_, node) == "!") {
            const uint32 swap = onTrue;
            onTrue = onFalse;
            onFalse = swap;
            node = tree_->Child(node, 0);
            continue;
        }

        break;
    }

    const StringView op = node == NoNode || tree_->Node(node).kind != SyntaxKind::Binary
                              ? StringView()
                              : checker_.TokenText(module_, node);
    if (op != "&&" && op != "||") {
        const uint32 value = Lower(node, checker_.boolType_);
        Add(IrOp::Branch, onTrue, value, onFalse, 0, node);
        return;
    }

    const bool andOperator = op == "&&";
    Array<uint32> terms(checker_.allocator_);
    uint32 leaf = node;
    while (leaf != NoNode && tree_->Node(leaf).kind == SyntaxKind::Binary &&
           checker_.TokenText(module_, leaf) == op) {
        if (auto pushed = terms.TryPushBack(tree_->Child(leaf, 1)); !pushed) {
            Fail(pushed.Error());
            return;
        }

        leaf = tree_->Child(leaf, 0);
    }

    if (auto pushed = terms.TryPushBack(leaf); !pushed) {
        Fail(pushed.Error());
        return;
    }

    // 원문 순서는 뒤에서부터다. 마지막 항은 바깥 갈래로 바로 간다.
    for (usize index = terms.Size(); index > 1; --index) {
        const uint32 next = NewBlock();
        LowerCondition(terms[index - 1], andOperator ? next : onTrue, andOperator ? onFalse : next);
        StartBlock(next);
    }

    LowerCondition(terms[0], onTrue, onFalse);
}

uint32 IrBuilder::LowerCoalesce(uint32 node, uint32 expected) noexcept {
    // a ?? b: a가 있으면 a의 값, 없으면 b.
    const uint32 resultType = TypeOf(node, expected);
    const uint32 leftNode = tree_->Child(node, 0);
    const uint32 left = Lower(leftNode, NoType);
    const uint32 result = NewValue(resultType);
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, left, NoIrRegister, 0, node);
    const uint32 hasValue = NewBlock();
    const uint32 otherwise = NewBlock();
    const uint32 done = NewBlock();
    Add(IrOp::Branch, hasValue, some, otherwise, 0, node);
    StartBlock(hasValue);
    MoveValue(result, Unwrap(left, resultType, node), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(otherwise);
    MoveValue(result, Coerce(Lower(tree_->Child(node, 1), resultType), resultType, node), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(done);
    return result;
}

uint32 IrBuilder::Unwrap(uint32 optional, uint32 type, uint32 node) noexcept {
    // T?의 값 묶음에서 T를 꺼낸다(Scalar는 있음 표지 뒤 slot, Ref·Buf는 같은 handle).
    if (optional == NoIrRegister) {
        return NoIrRegister;
    }

    const uint32 result = NewValue(type);
    if (result == NoIrRegister) {
        return NoIrRegister;
    }

    const bool flagged = function_.registers[optional].scalar == IrScalar::Flag &&
                         SlotCount(optional) == SlotCount(result) + 1;
    for (uint32 slot = 0; slot < SlotCount(result); ++slot) {
        Add(IrOp::Move, result + slot, optional + slot + (flagged ? 1 : 0), NoIrRegister, 0, node);
    }

    return result;
}

uint32 IrBuilder::LowerUnary(uint32 node, uint32 expected) noexcept {
    const StringView op = checker_.TokenText(module_, node);
    const uint32 type = TypeOf(node, expected);
    const uint32 operand = Lower(tree_->Child(node, 0), type);
    const uint32 result = NewValue(type);
    for (uint32 slot = 0; slot < SlotCount(result) && operand != NoIrRegister; ++slot) {
        Add(IrOp::Unary, result + slot, operand + slot, NoIrRegister, 0, node,
            static_cast<uint8>(op == "!" ? IrArith::Not : IrArith::Neg));
    }

    return result;
}

} // namespace rocks::sparks::detail
