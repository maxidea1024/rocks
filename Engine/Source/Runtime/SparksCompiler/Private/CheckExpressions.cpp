#include <Base/NumericLimits.h>
#include <SparksCompiler/Literal.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "TypeChecker.h"

// 식의 타입이다(Docs/Design/SparksLanguage.ko.md 3.10절): 리터럴·이름·연산자·상수식·색인·보간·
// 컬렉션·`as?`·`await`·`try`와 기대 타입 맞추기(대입 가능, 상수 범위).

namespace rocks::sparks::detail {

namespace {

// 타입 없는 정수 상수의 범위(SD66): −2^63..2^64−1.
constexpr uint64 MaxNegativeMagnitude = uint64(1) << 63;

// float32·float64가 정수를 정확히 담는 가수 bit 수다.
constexpr uint32 Float32Mantissa = 24;
constexpr uint32 Float64Mantissa = 53;

// 정수 상수의 덧셈이다(부호·크기). 범위를 벗어나면 false.
bool AddConstant(ConstantValue& out, bool negative, uint64 magnitude) noexcept {
    if (out.negative == negative) {
        const uint64 sum = out.magnitude + magnitude;
        if (sum < out.magnitude || (negative && sum > MaxNegativeMagnitude)) {
            return false;
        }

        out.magnitude = sum;
        return true;
    }

    if (out.magnitude >= magnitude) {
        out.magnitude -= magnitude;
    } else {
        out.magnitude = magnitude - out.magnitude;
        out.negative = negative;
    }

    if (out.magnitude == 0) {
        out.negative = false;
    }

    return true;
}

bool MultiplyConstant(ConstantValue& out, bool negative, uint64 magnitude) noexcept {
    const bool resultNegative = out.negative != negative;
    if (out.magnitude != 0 && magnitude > NumericLimits<uint64>::Max() / out.magnitude) {
        return false;
    }

    const uint64 product = out.magnitude * magnitude;
    if (resultNegative && product > MaxNegativeMagnitude) {
        return false;
    }

    out.magnitude = product;
    out.negative = resultNegative && product != 0;
    return true;
}

// 내림 나머지(`%`, 3.3절)다. 0 나누기는 부르는 쪽이 막는다.
ConstantValue RemainderConstant(const ConstantValue& left, const ConstantValue& right) noexcept {
    ConstantValue result;
    result.kind = ConstantValue::Kind::Integer;
    uint64 remainder = left.magnitude % right.magnitude;
    if (remainder != 0 && left.negative != right.negative) {
        remainder = right.magnitude - remainder;
    }

    result.magnitude = remainder;
    result.negative = right.negative && remainder != 0;
    return result;
}

double ToReal(const ConstantValue& constant) noexcept {
    if (constant.kind == ConstantValue::Kind::Float) {
        return constant.real;
    }

    const double magnitude = static_cast<double>(constant.magnitude);
    return constant.negative ? -magnitude : magnitude;
}

bool IsFinite(double value) noexcept {
    return value == value && value <= NumericLimits<double>::Max() &&
           value >= -NumericLimits<double>::Max();
}

} // namespace

StringView TypeChecker::TokenText(uint32 module, uint32 node) const noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    const SourceSpan span = tree.Tokens()[tree.Node(node).token].span;
    const StringView text = compilation_.sources_->Get(compilation_.modules_[module].source).Text();
    return StringView(text.Data() + span.begin, span.end - span.begin);
}

void TypeChecker::Record(const CheckContext& context, uint32 node, uint32 type) noexcept {
    const uint32 base = compilation_.nodeBase_[context.module];
    if (base != NoNode && node != NoNode) {
        compilation_.exprTypes_[base + node] = type;
    }
}

void TypeChecker::ReportAt(const CheckContext& context, DiagnosticCode code, SourceSpan span,
                           StringView message, StringView explanation, StringView fix) noexcept {
    binder_.Report(code, compilation_.modules_[context.module].source, span, message, explanation,
                   fix);
}

void TypeChecker::Report(const CheckContext& context, DiagnosticCode code, uint32 node,
                         StringView message, StringView explanation, StringView fix) noexcept {
    ReportAt(context, code, compilation_.trees_[context.module].Node(node).span, message,
             explanation, fix);
}

void TypeChecker::ReportMismatch(const CheckContext& context, uint32 node, uint32 from,
                                 uint32 to) noexcept {
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.Append("A value of type '");
    static_cast<void>(compilation_.AppendTypeText(message, from));
    message.Append("' cannot be used as '");
    static_cast<void>(compilation_.AppendTypeText(message, to));
    message.Append("'.");
    const bool numbers = Numeric(from).kind != NumericKind::None &&
                         Numeric(NonOptional(to)).kind != NumericKind::None;
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::TypeMismatch, node, message.View(),
               numbers ? StringView("Numbers of different types are never converted silently.")
                       : StringView("The value must have that type or be derived from it."),
               numbers ? StringView("Convert it, as in 'float(x)' or 'int32(x)'.")
                       : StringView("Use a value of that type."));
    }
}

bool TypeChecker::RequireValue(const CheckContext& context, const ExpressionValue& value,
                               uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    StringView message;
    DiagnosticCode code = DiagnosticCode::NotAValue;
    StringView fix = "Use a value here.";
    switch (value.kind) {
    case K::Error:
        return false;
    case K::Typed:
    case K::UntypedInt:
    case K::UntypedFloat:
    case K::Null:
        return true;
    case K::Void:
        message = "This call returns nothing, so it has no value.";
        break;
    case K::Wait:
    case K::Event:
        code = DiagnosticCode::Unawaited;
        message = "This must be awaited before its value is used.";
        fix = "Write 'await' before it.";
        break;
    case K::TypeName:
        message = "A type is not a value.";
        fix = "Call it to create a value, or use a value of that type.";
        break;
    case K::Namespace:
        message = "This name is a namespace, not a value.";
        fix = "Use one of its members.";
        break;
    case K::Function:
        message = "A function is not a value.";
        fix = "Call it, as in 'F()'.";
        break;
    case K::Literal:
        message = "A validated literal is written as a call.";
        fix = "Write it as in 'Tag(\"Item.Key\")'.";
        break;
    }

    Report(context, code, node, message, "", fix);
    return false;
}

bool TypeChecker::ConstantFits(const ConstantValue& constant, bool integer, uint32 type) const noexcept {
    const NumericInfo numeric = Numeric(type);
    if (numeric.kind == NumericKind::None) {
        return false;
    }

    if (!integer) {
        if (numeric.kind != NumericKind::Float) {
            return false;
        }

        return numeric.bits == 64 || (constant.real <= NumericLimits<float>::Max() &&
                                      constant.real >= -NumericLimits<float>::Max());
    }

    if (numeric.kind == NumericKind::Float) {
        const uint32 mantissa = numeric.bits == 32 ? Float32Mantissa : Float64Mantissa;
        if (constant.magnitude <= (uint64(1) << mantissa)) {
            return true;
        }

        // 큰 값은 하위 bit가 0이어서 가수에 정확히 들면 된다.
        uint64 magnitude = constant.magnitude;
        while ((magnitude & 1) == 0) {
            magnitude >>= 1;
        }

        return magnitude <= (uint64(1) << mantissa);
    }

    if (numeric.kind == NumericKind::Unsigned) {
        return !constant.negative &&
               (numeric.bits == 64 || constant.magnitude <= (uint64(1) << numeric.bits) - 1);
    }

    const uint64 limit = uint64(1) << (numeric.bits - 1);
    return constant.negative ? constant.magnitude <= limit : constant.magnitude < limit;
}

uint32 TypeChecker::Coerce(const CheckContext& context, const ExpressionValue& value,
                           uint32 type, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    if (type == NoType) {
        return Materialize(context, value, node);
    }

    switch (value.kind) {
    case K::Error:
        return NoType;
    case K::Typed:
        if (!IsAssignable(value.type, type)) {
            ReportMismatch(context, node, value.type, type);
            return NoType;
        }

        return type;
    case K::UntypedInt:
    case K::UntypedFloat: {
        const uint32 target = NonOptional(type);
        const bool integer = value.kind == K::UntypedInt;
        if (Numeric(target).kind == NumericKind::None) {
            ReportMismatch(context, node, integer ? intType_ : floatType_, type);
            return NoType;
        }

        if (!ConstantFits(value.constant, integer, target)) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            const bool floatInInteger = !integer && Numeric(target).kind != NumericKind::Float;
            if (floatInInteger) {
                message.Append("A floating-point constant cannot be used as '");
            } else {
                message.Append("The constant does not fit in '");
            }

            static_cast<void>(compilation_.AppendTypeText(message, target));
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context,
                       floatInInteger ? DiagnosticCode::FloatConstantInInteger
                                      : DiagnosticCode::IntegerLiteralTooLarge,
                       node, message.View(),
                       floatInInteger ? StringView("Integers are never made from fractions "
                                                   "silently.")
                                      : StringView("A constant must fit in the type of its "
                                                   "place."),
                       floatInInteger ? StringView("Write an integer, or use 'Math.RoundToInt'.")
                                      : StringView("Use a value in range or a wider type."));
            }

            return NoType;
        }

        Record(context, node, type);
        return type;
    }
    case K::Null:
        if (!IsOptional(type)) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("'null' cannot be used as '");
            static_cast<void>(compilation_.AppendTypeText(message, type));
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::TypeMismatch, node, message.View(),
                       "Only a type with '?' has no value.", "Declare the type with '?'.");
            }

            return NoType;
        }

        Record(context, node, type);
        return type;
    default:
        RequireValue(context, value, node);
        return NoType;
    }
}

uint32 TypeChecker::Materialize(const CheckContext& context, const ExpressionValue& value,
                                uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    switch (value.kind) {
    case K::Typed:
        return value.type;
    case K::UntypedInt:
        return Coerce(context, value, intType_, node);
    case K::UntypedFloat:
        return Coerce(context, value, floatType_, node);
    case K::Null:
        Report(context, DiagnosticCode::NoExpectedType, node,
               "'null' needs a type here.", "Nothing tells which type the value has.",
               "Declare the type, as in 'var target: Actor? = null;'.");
        return NoType;
    default:
        RequireValue(context, value, node);
        return NoType;
    }
}

ExpressionValue TypeChecker::Check(const CheckContext& context, uint32 node,
                                   uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    ExpressionValue result;
    if (node == NoNode || binder_.error_ != ErrorCode::None) {
        return result;
    }

    const SyntaxTree& tree = compilation_.trees_[context.module];
    const SyntaxNode& entry = tree.Node(node);
    switch (entry.kind) {
    case SyntaxKind::IntegerLiteral: {
        auto decoded = TryDecodeIntegerLiteral(TokenText(context.module, node));
        if (!decoded) {
            Report(context, DiagnosticCode::IntegerLiteralTooLarge, node,
                   "The integer literal is larger than any integer type.",
                   "The largest integer value is 18446744073709551615 (uint64).",
                   "Use a smaller value.");
            return result;
        }

        result.kind = K::UntypedInt;
        result.constant.kind = ConstantValue::Kind::Integer;
        result.constant.magnitude = decoded.Value();
        return result;
    }
    case SyntaxKind::FloatLiteral: {
        auto decoded =
            TryDecodeFloatLiteral(allocator_, TokenText(context.module, node));
        if (!decoded) {
            if (decoded.Error() == ErrorCode::OutOfMemory) {
                binder_.error_ = decoded.Error();
            }

            return result; // 범위 밖(S0113)은 어휘 분석이 알렸다
        }

        result.kind = K::UntypedFloat;
        result.constant.kind = ConstantValue::Kind::Float;
        result.constant.real = decoded.Value();
        return result;
    }
    case SyntaxKind::BoolLiteral:
        result.kind = K::Typed;
        result.type = boolType_;
        result.constant.kind = ConstantValue::Kind::Bool;
        result.constant.boolean = TokenText(context.module, node) == "true";
        Record(context, node, boolType_);
        return result;
    case SyntaxKind::NullLiteral:
        result.kind = K::Null;
        return result;
    case SyntaxKind::String:
    case SyntaxKind::VerbatimString:
        return CheckString(context, node);
    case SyntaxKind::Identifier:
        result = CheckIdentifier(context, node);
        break;
    case SyntaxKind::This:
        if (context.thisType == NoType) {
            Report(context, DiagnosticCode::ThisMisuse, node,
                   "'this' exists only in the functions and handlers of a class.",
                   "Struct functions, top-level functions and constants have no 'this'.",
                   "Remove 'this'.");
            return result;
        }

        result.kind = K::Typed;
        result.type = context.thisType;
        break;
    case SyntaxKind::Member:
        result = CheckMember(context, node);
        break;
    case SyntaxKind::Call:
        result = CheckCall(context, node, expected);
        break;
    case SyntaxKind::Index:
        result = CheckIndex(context, node);
        break;
    case SyntaxKind::Binary:
        result = CheckBinary(context, node, expected);
        break;
    case SyntaxKind::Unary:
        result = CheckUnary(context, node, expected);
        break;
    case SyntaxKind::CastTest:
        result = CheckCast(context, node);
        break;
    case SyntaxKind::Try:
    case SyntaxKind::Await:
        result = CheckAwaitTry(context, node);
        break;
    case SyntaxKind::Parenthesized:
        result = Check(context, tree.Child(node, 0), expected);
        if (result.kind == K::Typed) {
            Record(context, node, result.type);
        }

        return result;
    case SyntaxKind::CollectionLiteral:
        result = CheckCollection(context, node, expected);
        break;
    default:
        return result;
    }

    if (result.kind == K::Typed) {
        Record(context, node, result.type);
    }

    return result;
}

ExpressionValue TypeChecker::CheckIdentifier(const CheckContext& context, uint32 node) noexcept {
    const uint32 symbol = compilation_.NodeSymbol(context.module, node);
    if (symbol == NoSymbol) {
        return ExpressionValue{};
    }

    return ValueOfSymbol(context, symbol, context.thisType, true);
}

ExpressionValue TypeChecker::ValueOfSymbol(const CheckContext& context, uint32 symbol,
                                           uint32 receiver, bool instance) noexcept {
    static_cast<void>(context);
    using K = ExpressionValue::Kind;
    ExpressionValue result;
    const Symbol& entry = compilation_.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::Local:
    case SymbolKind::Parameter:
    case SymbolKind::EngineField:
    case SymbolKind::ContextName:
        if (entry.type == NoType) {
            return result;
        }

        result.kind = K::Typed;
        result.type = entry.type;
        result.symbol = symbol;
        return result;
    case SymbolKind::Field: {
        const uint32 type = FieldType(symbol);
        if (type == NoType) {
            return result;
        }

        result.kind = K::Typed;
        result.type = type;
        result.symbol = symbol;
        return result;
    }
    case SymbolKind::LanguageField: {
        const uint32 type = Substitute(entry.type, receiver, Span<const uint32>(), NoType);
        if (type == NoType) {
            return result;
        }

        result.kind = K::Typed;
        result.type = type;
        result.symbol = symbol;
        return result;
    }
    case SymbolKind::Const:
        if (entry.type == NoType) {
            return result;
        }

        result.kind = K::Typed;
        result.type = entry.type;
        result.symbol = symbol;
        result.constant = ConstOf(symbol);
        return result;
    case SymbolKind::EnumCase: {
        result.kind = K::Typed;
        result.type = TypeOfSymbol(entry.owner);
        result.symbol = symbol;
        result.constant.kind = ConstantValue::Kind::Enum;
        result.constant.symbol = symbol;
        return result;
    }
    case SymbolKind::Method:
    case SymbolKind::EngineFunction:
    case SymbolKind::LanguageFunction:
    case SymbolKind::Function:
    case SymbolKind::InterfaceMethod:
        result.kind = K::Function;
        result.symbol = symbol;
        result.receiver = receiver;
        result.instance = instance && (entry.flags & SymbolFlags::Static) == 0;
        return result;
    case SymbolKind::Event:
    case SymbolKind::EngineEvent:
    case SymbolKind::LanguageEvent:
        result.kind = K::Event;
        result.symbol = symbol;
        result.receiver = receiver;
        return result;
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
    case SymbolKind::EngineType:
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType:
        result.kind = K::TypeName;
        result.symbol = symbol;
        return result;
    case SymbolKind::PreludeValue:
        if (entry.signature != NoSignature) {
            result.kind = K::Function;
            result.symbol = symbol;
            return result;
        }

        result.kind = LanguageNames()[entry.index].kind == LanguageNameKind::Literal
                          ? K::Literal
                          : K::Namespace;
        result.symbol = symbol;
        return result;
    case SymbolKind::Alias:
        result.kind = K::Namespace;
        result.symbol = symbol;
        return result;
    default:
        return result; // RPC 접두(LC3) 등
    }
}

ExpressionValue TypeChecker::CheckBinary(const CheckContext& context, uint32 node,
                                         uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const StringView op = TokenText(context.module, node);
    const uint32 leftNode = tree.Child(node, 0);
    const uint32 rightNode = tree.Child(node, 1);
    ExpressionValue result;
    auto typed = [&](uint32 type) noexcept {
        ExpressionValue value;
        value.kind = type == NoType ? K::Error : K::Typed;
        value.type = type;
        return value;
    };

    if (op == "&&" || op == "||") {
        bool fine = true;
        const uint32 sides[] = {leftNode, rightNode};
        for (const uint32 side : sides) {
            const ExpressionValue value = Check(context, side, boolType_);
            if (value.kind == K::Error) {
                fine = false;
                continue;
            }

            if (!(value.kind == K::Typed && value.type == boolType_)) {
                if (RequireValue(context, value, side)) {
                    Report(context, DiagnosticCode::ConditionNotBool, side,
                           "The operand of '&&' and '||' must be 'bool'.",
                           "Conditions are never made from other types silently.",
                           "Compare it, as in 'x != null'.");
                }

                fine = false;
            }
        }

        return fine ? typed(boolType_) : result;
    }

    if (op == "??") {
        const ExpressionValue left = Check(context, leftNode, MakeOptional(expected));
        if (left.kind == K::Error) {
            static_cast<void>(Check(context, rightNode, NoType));
            return result;
        }

        if (left.kind != K::Typed || !IsOptional(left.type)) {
            if (RequireValue(context, left, leftNode)) {
                Report(context, DiagnosticCode::NotOptional, leftNode,
                       "The left side of '?" "?' must have a type with '?'.",
                       "'?" "?' chooses the right side when the left side is null.",
                       "Remove '?? ...'.");
            }

            static_cast<void>(Check(context, rightNode, NoType));
            return result;
        }

        const uint32 inner = NonOptional(left.type);
        const ExpressionValue right = Check(context, rightNode, inner);
        if (right.kind == K::Typed && IsOptional(right.type)) {
            if (!IsAssignable(right.type, left.type)) {
                ReportMismatch(context, rightNode, right.type, left.type);
                return result;
            }

            return typed(left.type);
        }

        return typed(Coerce(context, right, inner, rightNode) == NoType ? NoType : inner);
    }

    // 나머지는 두 피연산자의 타입을 맞춘다(한쪽이 상수·null이면 다른 쪽 타입으로).
    ExpressionValue left = Check(context, leftNode, NoType);
    const uint32 leftHint = left.kind == K::Typed ? left.type : NoType;
    ExpressionValue right = Check(context, rightNode, leftHint);
    if (left.kind == K::Error || right.kind == K::Error) {
        return result;
    }

    if (!RequireValue(context, left, leftNode) || !RequireValue(context, right, rightNode)) {
        return result;
    }

    const bool arithmetic = op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
    const bool untypedLeft = left.kind == K::UntypedInt || left.kind == K::UntypedFloat;
    const bool untypedRight = right.kind == K::UntypedInt || right.kind == K::UntypedFloat;
    // 상수끼리의 산술은 상수식이다(SD66).
    if (arithmetic && untypedLeft && untypedRight) {
        const bool integer = left.kind == K::UntypedInt && right.kind == K::UntypedInt;
        if (op == "/" && integer) {
            Report(context, DiagnosticCode::IntegerDivision, node,
                   "Integers are not divided with '/'.",
                   "Integer division drops the fraction, which is easy to miss.",
                   "Use 'Math.Div(a, b)' for the quotient or 'float(a) / float(b)'.");
            return result;
        }

        if (op == "%" && !integer) {
            Report(context, DiagnosticCode::InvalidOperator, node,
                   "'%' is only for integers.", "", "Use integers.");
            return result;
        }

        ExpressionValue folded;
        if (integer) {
            folded.kind = K::UntypedInt;
            folded.constant = left.constant;
            bool fine = true;
            if (op == "+") {
                fine = AddConstant(folded.constant, right.constant.negative,
                                   right.constant.magnitude);
            } else if (op == "-") {
                fine = AddConstant(folded.constant,
                                   !right.constant.negative && right.constant.magnitude != 0,
                                   right.constant.magnitude);
            } else if (op == "*") {
                fine = MultiplyConstant(folded.constant, right.constant.negative,
                                        right.constant.magnitude);
            } else {
                if (right.constant.magnitude == 0) {
                    Report(context, DiagnosticCode::ConstantDivisionByZero, node,
                           "The constant is divided by zero.", "", "Use a value other than 0.");
                    return result;
                }

                folded.constant = RemainderConstant(left.constant, right.constant);
            }

            if (!fine) {
                Report(context, DiagnosticCode::ConstantOverflow, node,
                       "The constant expression overflows.",
                       "Integer constants are computed exactly between -2^63 and 2^64-1.",
                       "Use smaller values.");
                return result;
            }

            return folded;
        }

        const double a = ToReal(left.constant);
        const double b = ToReal(right.constant);
        const double value = op == "+" ? a + b : op == "-" ? a - b : op == "*" ? a * b : a / b;
        if (!IsFinite(value)) {
            Report(context, DiagnosticCode::NonFiniteConstant, node,
                   "The constant expression is not a finite number.",
                   "Constant arithmetic that makes NaN or infinity is an error.",
                   "Change the values.");
            return result;
        }

        folded.kind = K::UntypedFloat;
        folded.constant.kind = ConstantValue::Kind::Float;
        folded.constant.real = value;
        return folded;
    }

    // 한쪽만 상수·null이면 다른 쪽 타입으로 맞춘다.
    uint32 leftType = NoType;
    uint32 rightType = NoType;
    if (left.kind == K::Typed) {
        leftType = left.type;
        rightType = right.kind == K::Typed ? right.type
                    : (op == "==" || op == "!=") && right.kind == K::Null
                        ? (IsOptional(leftType) ? leftType : NoType)
                        : Coerce(context, right, IsVector(leftType) ? floatType_ : leftType,
                                 rightNode);
    } else if (right.kind == K::Typed) {
        rightType = right.type;
        leftType = (op == "==" || op == "!=") && left.kind == K::Null
                       ? (IsOptional(rightType) ? rightType : NoType)
                       : Coerce(context, left, IsVector(rightType) ? floatType_ : rightType,
                                leftNode);
    } else {
        leftType = Materialize(context, left, leftNode);
        rightType = Materialize(context, right, rightNode);
    }

    const bool nullComparison = (left.kind == K::Null || right.kind == K::Null);
    if (op == "==" || op == "!=") {
        if (nullComparison) {
            if (leftType == NoType && rightType == NoType) {
                Report(context, DiagnosticCode::NotComparable, node,
                       "Only a value whose type has '?' can be compared with 'null'.",
                       "A value of a type without '?' is never null.", "Remove the comparison.");
                return result;
            }

            return typed(boolType_);
        }

        if (leftType == NoType || rightType == NoType) {
            return result;
        }

        const bool related = IsAssignable(leftType, rightType) || IsAssignable(rightType, leftType);
        if (!related || !IsEquatable(NonOptional(leftType))) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("Values of type '");
            static_cast<void>(compilation_.AppendTypeText(message, leftType));
            message.Append("' and '");
            static_cast<void>(compilation_.AppendTypeText(message, rightType));
            message.Append("' cannot be compared with '").Append(op).Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context,
                       Numeric(leftType).kind != NumericKind::None &&
                               Numeric(rightType).kind != NumericKind::None && !related
                           ? DiagnosticCode::MixedNumericTypes
                           : DiagnosticCode::NotComparable,
                       node, message.View(),
                       "'==' is for booleans, integers, strings, enums, tags, objects and their "
                       "'?' types; floats and vectors use 'Math.NearlyEqual'.",
                       "Compare values of one comparable type.");
            }

            return result;
        }

        return typed(boolType_);
    }

    if (leftType == NoType || rightType == NoType) {
        return result;
    }

    const NumericInfo leftNumeric = Numeric(leftType);
    const NumericInfo rightNumeric = Numeric(rightType);
    if (leftNumeric.kind != NumericKind::None && rightNumeric.kind != NumericKind::None &&
        leftType != rightType) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("'").Append(op).Append("' mixes '");
        static_cast<void>(compilation_.AppendTypeText(message, leftType));
        message.Append("' and '");
        static_cast<void>(compilation_.AppendTypeText(message, rightType));
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::MixedNumericTypes, node, message.View(),
                   "Numbers of different types are never mixed silently.",
                   "Convert one side, as in 'float(x)'.");
        }

        return result;
    }

    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        if (leftType == rightType && IsOrdered(leftType)) {
            return typed(boolType_);
        }
    } else if (leftNumeric.kind != NumericKind::None && leftType == rightType) {
        if (op == "/" && leftNumeric.kind != NumericKind::Float) {
            Report(context, DiagnosticCode::IntegerDivision, node,
                   "Integers are not divided with '/'.",
                   "Integer division drops the fraction, which is easy to miss.",
                   "Use 'Math.Div(a, b)' for the quotient or 'float(a) / float(b)'.");
            return result;
        }

        if (op == "%" && leftNumeric.kind == NumericKind::Float) {
            Report(context, DiagnosticCode::InvalidOperator, node, "'%' is only for integers.",
                   "", "Use integers.");
            return result;
        }

        return typed(leftType);
    } else if ((op == "+" || op == "-") && IsVector(leftType) && leftType == rightType) {
        return typed(leftType);
    } else if (op == "*" && IsVector(leftType) && rightType == floatType_) {
        return typed(leftType);
    } else if (op == "*" && leftType == floatType_ && IsVector(rightType)) {
        return typed(rightType);
    } else if (op == "/" && IsVector(leftType) && rightType == floatType_) {
        return typed(leftType);
    } else if (op == "*" && leftType == rightType &&
               (IsLanguage(leftType, "quat") || IsLanguage(leftType, "mat4")) &&
               !IsOptional(leftType)) {
        return typed(leftType);
    }

    const bool strings = op == "+" && IsLanguage(leftType, "string");
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.Append("'").Append(op).Append("' is not defined for '");
    static_cast<void>(compilation_.AppendTypeText(message, leftType));
    message.Append("' and '");
    static_cast<void>(compilation_.AppendTypeText(message, rightType));
    message.Append("'.");
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
               strings ? StringView("Strings are joined with interpolation, not '+'.")
                       : StringView(""),
               strings ? StringView("Write \"${a}${b}\".")
                       : StringView("Use operands of a type that has the operator."));
    }

    return result;
}

ExpressionValue TypeChecker::CheckUnary(const CheckContext& context, uint32 node,
                                        uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const StringView op = TokenText(context.module, node);
    const uint32 operandNode = tree.Child(node, 0);
    ExpressionValue result;
    if (op == "!") {
        const ExpressionValue operand = Check(context, operandNode, boolType_);
        if (operand.kind == K::Error) {
            return result;
        }

        if (!(operand.kind == K::Typed && operand.type == boolType_)) {
            if (RequireValue(context, operand, operandNode)) {
                Report(context, DiagnosticCode::ConditionNotBool, operandNode,
                       "The operand of '!' must be 'bool'.",
                       "Conditions are never made from other types silently.",
                       "Compare it, as in 'x == 0'.");
            }

            return result;
        }

        result.kind = K::Typed;
        result.type = boolType_;
        return result;
    }

    ExpressionValue operand = Check(context, operandNode, expected);
    if (operand.kind == K::UntypedInt) {
        ExpressionValue negated = operand;
        negated.constant.negative = !operand.constant.negative && operand.constant.magnitude != 0;
        if (negated.constant.negative && negated.constant.magnitude > MaxNegativeMagnitude) {
            Report(context, DiagnosticCode::ConstantOverflow, node,
                   "The constant expression overflows.",
                   "Integer constants are computed exactly between -2^63 and 2^64-1.",
                   "Use smaller values.");
            return result;
        }

        return negated;
    }

    if (operand.kind == K::UntypedFloat) {
        operand.constant.real = -operand.constant.real;
        return operand;
    }

    if (operand.kind == K::Error || !RequireValue(context, operand, operandNode)) {
        return result;
    }

    const uint32 type = Materialize(context, operand, operandNode);
    const NumericInfo numeric = Numeric(type);
    if (numeric.kind == NumericKind::Unsigned) {
        Report(context, DiagnosticCode::UnsignedNegation, node,
               "An unsigned integer cannot be negated.", "The result could never be stored.",
               "Use a signed type.");
        return result;
    }

    if (numeric.kind == NumericKind::None && !IsVector(type)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("Unary '-' is not defined for '");
        static_cast<void>(compilation_.AppendTypeText(message, type));
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::InvalidOperator, node, message.View(), "",
                   "Use a number or a vector.");
        }

        return result;
    }

    result.kind = K::Typed;
    result.type = type;
    return result;
}

ExpressionValue TypeChecker::CheckIndex(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    const ExpressionValue object = Check(context, tree.Child(node, 0), NoType);
    if (object.kind == K::Error || !RequireValue(context, object, tree.Child(node, 0))) {
        static_cast<void>(Check(context, tree.Child(node, 1), NoType));
        return result;
    }

    const uint32 type = Materialize(context, object, tree.Child(node, 0));
    if (IsOptional(type)) {
        Report(context, DiagnosticCode::MayBeNull, tree.Child(node, 0),
               "The value may be null, so it cannot be indexed.",
               "A value whose type has '?' must be checked first.",
               "Use 'if (let x = value) { ... }'.");
        static_cast<void>(Check(context, tree.Child(node, 1), NoType));
        return result;
    }

    uint32 key = NoType;
    uint32 element = NoType;
    if (IsLanguage(type, "Array")) {
        key = intType_;
        element = Argument(type, 0);
    } else if (IsLanguage(type, "Map")) {
        key = Argument(type, 0);
        element = Argument(type, 1);
    } else {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("A value of type '");
        static_cast<void>(compilation_.AppendTypeText(message, type));
        message.Append("' cannot be indexed.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
                   "Only arrays and maps are indexed.", "Use an array or a map.");
        }

        static_cast<void>(Check(context, tree.Child(node, 1), NoType));
        return result;
    }

    const ExpressionValue index = Check(context, tree.Child(node, 1), key);
    if (Coerce(context, index, key, tree.Child(node, 1)) == NoType) {
        return result;
    }

    result.kind = K::Typed;
    result.type = MakeOptional(element);
    return result;
}

ExpressionValue TypeChecker::CheckString(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    result.kind = K::Typed;
    result.type = stringType_;
    bool interpolated = false;
    if (tree.Node(node).kind == SyntaxKind::String) {
        for (const uint32 part : tree.Children(node)) {
            if (tree.Node(part).kind != SyntaxKind::Interpolation) {
                continue;
            }

            interpolated = true;
            const uint32 inner = tree.Child(part, 0);
            const ExpressionValue value = Check(context, inner, NoType);
            if (value.kind == K::Error || !RequireValue(context, value, inner)) {
                continue;
            }

            const uint32 type = Materialize(context, value, inner);
            if (type != NoType && !IsInterpolatable(type)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("A value of type '");
                static_cast<void>(compilation_.AppendTypeText(message, type));
                message.Append("' cannot be interpolated.");
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::InterpolationType, inner, message.View(),
                           "Interpolation shows booleans, numbers, strings, enums, tags and math "
                           "types; values with '?', objects, structs, containers and Text are "
                           "not shown directly.",
                           "Interpolate a field of it or check it for null first.");
                }
            }
        }
    }

    if (!interpolated) {
        result.constant.kind = ConstantValue::Kind::String;
        result.constant.text = StringView(
            compilation_.sources_->Get(compilation_.modules_[context.module].source).Text().Data() +
                tree.Node(node).span.begin,
            tree.Node(node).span.end - tree.Node(node).span.begin);
    }

    Record(context, node, stringType_);
    return result;
}

ExpressionValue TypeChecker::CheckCast(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    const uint32 operandNode = tree.Child(node, 0);
    const ExpressionValue operand = Check(context, operandNode, NoType);
    const uint32 target = compilation_.NodeType(context.module, tree.Child(node, 1));
    if (operand.kind == K::Error || target == NoType ||
        !RequireValue(context, operand, operandNode)) {
        return result;
    }

    const uint32 source = NonOptional(Materialize(context, operand, operandNode));
    if (source == NoType) {
        return result;
    }

    const Symbol& targetSymbol = compilation_.symbols_[SymbolOf(target)];
    const bool targetClass = targetSymbol.kind == SymbolKind::Class ||
                             targetSymbol.kind == SymbolKind::Interface ||
                             (targetSymbol.kind == SymbolKind::EngineType && IsReference(target));
    const bool targetInterface =
        targetSymbol.kind == SymbolKind::Interface ||
        (targetSymbol.kind == SymbolKind::EngineType &&
         compilation_.schemas_[targetSymbol.index].schema.Types()[targetSymbol.item].kind ==
             SchemaTypeKind::Interface);
    const Symbol& sourceSymbol = compilation_.symbols_[SymbolOf(source)];
    const bool sourceInterface =
        sourceSymbol.kind == SymbolKind::Interface ||
        (sourceSymbol.kind == SymbolKind::EngineType && IsReference(source) &&
         compilation_.schemas_[sourceSymbol.index].schema.Types()[sourceSymbol.item].kind ==
             SchemaTypeKind::Interface);
    StringView problem;
    if (!IsReference(source)) {
        problem = "'as?' needs an object value.";
    } else if (!targetClass) {
        problem = "'as?' converts to a class or interface type.";
    } else if (IsDerived(source, target)) {
        problem = "The value already has that type, so 'as?' is not needed.";
    } else if (!targetInterface && !sourceInterface && !IsDerived(target, source)) {
        problem = "The two classes are unrelated, so the conversion never succeeds.";
    }

    if (!problem.IsEmpty()) {
        Report(context, DiagnosticCode::InvalidCast, node, problem,
               "'as?' converts down to a derived class or asks for an interface.",
               "Remove 'as?' or fix the type.");
        return result;
    }

    result.kind = K::Typed;
    result.type = MakeOptional(target);
    return result;
}

ExpressionValue TypeChecker::CheckAwaitTry(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const SyntaxNode& entry = tree.Node(node);
    const bool isTry = entry.kind == SyntaxKind::Try;
    const bool awaits = !isTry || (entry.flags & SyntaxFlags::Await) != 0;
    const uint32 operandNode = tree.Child(node, 0);
    ExpressionValue value = Check(context, operandNode, NoType);
    ExpressionValue result;
    if (value.kind == K::Error) {
        return result;
    }

    if (awaits) {
        ExpressionValue awaited;
        if (value.kind == K::Wait) {
            awaited.kind = value.type == voidType_ ? K::Void : K::Typed;
            awaited.type = value.type;
        } else if (value.kind == K::Event) {
            awaited.kind = K::Void;
        } else if (value.kind == K::Typed && IsLanguage(value.type, "LoadRequest") &&
                   !IsOptional(value.type)) {
            const uint32 element = Argument(value.type, 0);
            const uint32 resultSymbol =
                binder_.FindName(SpaceKey(NameSpaceKind::Language, 0), "Result");
            awaited.kind = K::Typed;
            awaited.type = binder_.InternType(resultSymbol, Span<const uint32>(&element, 1), false);
        } else {
            if (RequireValue(context, value, operandNode)) {
                Report(context, DiagnosticCode::AwaitTryOperand, node,
                       "Only a wait can be awaited.",
                       "'await' waits for an async function call, a delay, an event or a load "
                       "request.",
                       "Remove 'await'.");
            }

            return result;
        }

        value = awaited;
    }

    if (!isTry) {
        return value;
    }

    if (!(value.kind == K::Typed && IsLanguage(value.type, "Result") && !IsOptional(value.type))) {
        if (value.kind == K::Void || RequireValue(context, value, operandNode)) {
            Report(context, DiagnosticCode::AwaitTryOperand, node,
                   "'try' needs a value of type 'Result'.",
                   "'try' takes the value out of a successful Result and stops on an error.",
                   "Remove 'try'.");
        }

        return result;
    }

    const uint32 inner = Argument(value.type, 0);
    if (inner == voidType_) {
        result.kind = K::Void;
        return result;
    }

    result.kind = K::Typed;
    result.type = inner;
    return result;
}

ExpressionValue TypeChecker::CheckCollection(const CheckContext& context, uint32 node,
                                             uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    const Span<const uint32> elements = tree.Children(node);
    const bool entries =
        !elements.IsEmpty() && tree.Node(elements[0]).kind == SyntaxKind::MapEntry;
    const uint32 target = NonOptional(expected);
    const bool isArray = IsLanguage(target, "Array");
    const bool isSet = IsLanguage(target, "Set");
    const bool isMap = IsLanguage(target, "Map");
    if (expected != NoType && (isArray || isSet || isMap)) {
        if (entries != isMap && !elements.IsEmpty()) {
            Report(context, DiagnosticCode::TypeMismatch, node,
                   isMap ? StringView("A map literal is written with 'key: value' entries.")
                         : StringView("An array or set literal has no 'key: value' entries."),
                   "", "Write the literal for the expected type.");
            return result;
        }

        for (const uint32 element : elements) {
            if (isMap) {
                if (tree.Node(element).kind != SyntaxKind::MapEntry) {
                    continue;
                }

                const uint32 keyNode = tree.Child(element, 0);
                const uint32 valueNode = tree.Child(element, 1);
                static_cast<void>(Coerce(context, Check(context, keyNode, Argument(target, 0)),
                                         Argument(target, 0), keyNode));
                static_cast<void>(Coerce(context, Check(context, valueNode, Argument(target, 1)),
                                         Argument(target, 1), valueNode));
            } else {
                static_cast<void>(Coerce(context, Check(context, element, Argument(target, 0)),
                                         Argument(target, 0), element));
            }
        }

        result.kind = K::Typed;
        result.type = target;
        return result;
    }

    if (elements.IsEmpty() || entries) {
        Report(context, DiagnosticCode::NoExpectedType, node,
               entries ? StringView("A map literal needs a type here.")
                       : StringView("An empty collection literal needs a type here."),
               "Nothing tells which collection type the literal makes.",
               "Declare the type, as in 'var scores: Map<string, int> = [];'.");
        for (const uint32 element : elements) {
            if (tree.Node(element).kind == SyntaxKind::MapEntry) {
                static_cast<void>(Check(context, tree.Child(element, 0), NoType));
                static_cast<void>(Check(context, tree.Child(element, 1), NoType));
            }
        }

        return result;
    }

    // 기대 타입이 없으면 원소가 모두 같은 타입이어야 한다(정수·실수 상수가 섞이면 float).
    Array<ExpressionValue> values(allocator_);
    uint32 elementType = NoType;
    bool anyFloat = false;
    bool anyInt = false;
    bool failed = false;
    for (const uint32 element : elements) {
        const ExpressionValue value = Check(context, element, elementType);
        if (auto pushed = values.TryPushBack(value); !pushed) {
            binder_.error_ = pushed.Error();
            return result;
        }

        if (value.kind == K::Error || !RequireValue(context, value, element)) {
            failed = true;
            continue;
        }

        anyFloat = anyFloat || value.kind == K::UntypedFloat;
        anyInt = anyInt || value.kind == K::UntypedInt;
        if (value.kind == K::Typed) {
            if (elementType == NoType) {
                elementType = value.type;
            } else if (value.type != elementType) {
                Report(context, DiagnosticCode::MixedCollectionTypes, element,
                       "The elements of the array literal have different types.",
                       "Elements are not combined into a base type.",
                       "Declare the array type, as in 'var items: Array<Actor> = [a, b];'.");
                return result;
            }
        }
    }

    if (failed) {
        return result;
    }

    if (elementType == NoType) {
        elementType = anyFloat ? floatType_ : intType_;
    }

    for (usize index = 0; index < elements.Size(); ++index) {
        if (values[index].kind != K::Typed &&
            Coerce(context, values[index], elementType, elements[index]) == NoType) {
            return result;
        }
    }

    static_cast<void>(anyInt);
    if (IsOptional(elementType)) {
        Report(context, DiagnosticCode::TypeMismatch, node,
               "An array element type cannot have '?'.", "Containers hold values, not nulls.",
               "Remove the null elements.");
        return result;
    }

    const uint32 arraySymbol = binder_.FindName(SpaceKey(NameSpaceKind::Language, 0), "Array");
    result.kind = K::Typed;
    result.type = binder_.InternType(arraySymbol, Span<const uint32>(&elementType, 1), false);
    return result;
}

} // namespace rocks::sparks::detail
