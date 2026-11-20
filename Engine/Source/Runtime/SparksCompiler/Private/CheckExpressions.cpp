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

// float32 최근접 짝수 반올림이 무한대가 되는 절댓값(2^128 - 2^103)이다. 이보다 작으면 유한하다.
constexpr double Float32RoundingLimit = 340282356779733661637539395458142568448.0;

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
    const uint32 token = tree.Node(node).token;
    if (token == NoToken) {
        return StringView(); // 구문 오류로 빠진 이름(`p.` 뒤) — 진단은 구문 분석이 냈다
    }

    const SourceSpan span = tree.Tokens()[token].span;
    const StringView text = compilation_.sources_->Get(compilation_.modules_[module].source).Text();
    return StringView(text.Data() + span.begin, span.end - span.begin);
}

void TypeChecker::Record(const CheckContext& context, uint32 node, uint32 type) noexcept {
    const uint32 base = compilation_.nodeBase_[context.module];
    if (base != NoNode && node != NoNode) {
        compilation_.exprTypes_[base + node] = type;
    }
}

bool TypeChecker::ReportZeroDivisor(const CheckContext& context, const ExpressionValue& divisor,
                                    uint32 node) noexcept {
    if (divisor.constant.kind != ConstantValue::Kind::Integer || divisor.constant.magnitude != 0) {
        return false;
    }

    Report(context, DiagnosticCode::ConstantDivisionByZero, node,
           "The constant is divided by zero.", "Dividing by zero always fails.",
           "Use a value other than 0.");
    return true;
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
    AppendDisplayType(message, from);
    message.Append("' cannot be used as '");
    AppendDisplayType(message, to);
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

bool TypeChecker::ConstantFits(const ConstantValue& constant, bool integer,
                               uint32 type) const noexcept {
    const NumericInfo numeric = Numeric(type);
    if (numeric.kind == NumericKind::None) {
        return false;
    }

    if (!integer) {
        if (numeric.kind != NumericKind::Float) {
            return false;
        }

        return numeric.bits == 64 ||
               (constant.real < Float32RoundingLimit && constant.real > -Float32RoundingLimit);
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

uint32 TypeChecker::Coerce(const CheckContext& context, const ExpressionValue& value, uint32 type,
                           uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    if (type == UnresolvedType) {
        return NoType; // 기대 타입을 모르는 자리(앞선 오류) — 더 알리지 않는다
    }

    if (type == NoType) {
        return Materialize(context, value, node);
    }

    // `void` 자리(잘못된 `void` 표기 — S0405는 CheckTypeRules)에는 더 진단하지 않는다.
    if (type == voidType_) {
        return NoType;
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

            AppendDisplayType(message, target);
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
            AppendDisplayType(message, type);
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
        Report(context, DiagnosticCode::NoExpectedType, node, "'null' needs a type here.",
               "Nothing tells which type the value has.",
               "Declare the type, as in 'var target: Actor? = null;'.");
        return NoType;
    default:
        RequireValue(context, value, node);
        return NoType;
    }
}

ExpressionValue TypeChecker::Check(const CheckContext& context, uint32 node,
                                   uint32 expected) noexcept {
    if (node == NoNode || binder_.error_ != ErrorCode::None) {
        return ExpressionValue{};
    }

    // 식은 왼쪽으로 깊게 이어질 수 있다(구문 트리 깊이 상한 1024 — 8.1절). Debug 빌드의 stack 1 MiB
    // 안에 들도록 재귀 경로의 frame에는 임시 값을 두지 않는다: 종류별 처리는 여기서 `return`으로
    // 부르고(반환 자리에 바로 만든다), 판정·진단·타입 기록은 재귀가 끝난 뒤 부르는 함수가 한다.
    // 기대 타입을 모르는 자리는 리터럴(컬렉션·검증 리터럴)만 구별하고 그 밖의 식에는 없음과 같다.
    const SyntaxTree& tree = compilation_.trees_[context.module];
    switch (tree.Node(node).kind) {
    case SyntaxKind::IntegerLiteral:
    case SyntaxKind::FloatLiteral:
    case SyntaxKind::BoolLiteral:
    case SyntaxKind::NullLiteral:
    case SyntaxKind::This:
        return CheckLeaf(context, node);
    case SyntaxKind::String:
    case SyntaxKind::VerbatimString:
        return CheckString(context, node);
    case SyntaxKind::Identifier:
        return CheckIdentifier(context, node);
    case SyntaxKind::Member:
        return CheckMember(context, node);
    case SyntaxKind::Call:
        return CheckCall(context, node, expected);
    case SyntaxKind::Index:
        return CheckIndex(context, node);
    case SyntaxKind::Binary:
        switch (tree.Tokens()[tree.Node(node).token].kind) {
        case TokenKind::AmpersandAmpersand:
        case TokenKind::PipePipe:
            return CheckLogical(context, node);
        case TokenKind::QuestionQuestion:
            return CheckCoalesce(context, node, expected == UnresolvedType ? NoType : expected);
        default:
            return CheckBinary(context, node);
        }
    case SyntaxKind::Unary:
        return CheckUnary(context, node, expected == UnresolvedType ? NoType : expected);
    case SyntaxKind::CastTest:
        return CheckCast(context, node);
    case SyntaxKind::Try:
    case SyntaxKind::Await:
        return CheckAwaitTry(context, node);
    case SyntaxKind::Parenthesized:
        return CheckParenthesized(context, node, expected);
    case SyntaxKind::CollectionLiteral:
        return CheckCollection(context, node, expected);
    default:
        return ExpressionValue{};
    }
}

ExpressionValue TypeChecker::CheckParenthesized(const CheckContext& context, uint32 node,
                                                uint32 expected) noexcept {
    const ExpressionValue value =
        Check(context, compilation_.trees_[context.module].Child(node, 0), expected);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CheckLeaf(const CheckContext& context, uint32 node) noexcept {
    using K = ExpressionValue::Kind;
    ExpressionValue result;
    switch (compilation_.trees_[context.module].Node(node).kind) {
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
        auto decoded = TryDecodeFloatLiteral(allocator_, TokenText(context.module, node));
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
        Record(context, node, result.type);
        return result;
    default:
        return result;
    }
}

ExpressionValue TypeChecker::CheckIdentifier(const CheckContext& context, uint32 node) noexcept {
    const uint32 symbol = compilation_.NodeSymbol(context.module, node);
    if (symbol == NoSymbol) {
        return ExpressionValue{};
    }

    const ExpressionValue value = ValueOfSymbol(context, symbol, context.thisType, true);
    if (value.kind != ExpressionValue::Kind::Function &&
        value.kind != ExpressionValue::Kind::Error &&
        RejectTypeArguments(context, compilation_.trees_[context.module].Child(node, 0),
                            compilation_.symbols_[symbol].name)) {
        return ExpressionValue{};
    }

    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

bool TypeChecker::RejectTypeArguments(const CheckContext& context, uint32 typeArguments,
                                      StringView name) noexcept {
    if (typeArguments == NoNode) {
        return false;
    }

    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.AppendFormat("'{}' takes no type arguments.", name);
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::TypeArgumentCount, typeArguments, message.View(),
               "Only generic functions take type arguments in expressions.",
               "Remove the type arguments.");
    }

    return true;
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

        result.kind = LanguageNames()[entry.index].kind == LanguageNameKind::Literal ? K::Literal
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

ExpressionValue TypeChecker::CheckBinary(const CheckContext& context, uint32 node) noexcept {
    // 왼쪽으로 깊은 연쇄(`a + b + c ...`)의 frame에는 두 값만 둔다(판정·기록은 CombineBinary).
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const ExpressionValue left = Check(context, tree.Child(node, 0), NoType);
    const ExpressionValue right =
        Check(context, tree.Child(node, 1),
              left.kind == ExpressionValue::Kind::Typed
                  ? left.type
                  : (left.kind == ExpressionValue::Kind::Error ? UnresolvedType : NoType));
    return CombineBinary(context, node, left, right);
}

ExpressionValue TypeChecker::CheckLogical(const CheckContext& context, uint32 node) noexcept {
    // 재귀 경로(`a && b && c ...`)의 frame에는 두 값만 둔다(CheckBinary 참고).
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const ExpressionValue left = Check(context, tree.Child(node, 0), boolType_);
    const ExpressionValue right = Check(context, tree.Child(node, 1), boolType_);
    return CombineLogical(context, node, left, right);
}

ExpressionValue TypeChecker::CombineLogical(const CheckContext& context, uint32 node,
                                            const ExpressionValue& left,
                                            const ExpressionValue& right) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CombineLogicalValue(context, node, left, right);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CombineLogicalValue(const CheckContext& context, uint32 node,
                                                 const ExpressionValue& left,
                                                 const ExpressionValue& right) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const StringView op = TokenText(context.module, node);
    bool fine = true;
    const ExpressionValue* values[] = {&left, &right};
    for (uint32 side = 0; side < 2; ++side) {
        const ExpressionValue& value = *values[side];
        const uint32 sideNode = tree.Child(node, side);
        if (value.kind == K::Error) {
            fine = false;
            continue;
        }

        if (!(value.kind == K::Typed && value.type == boolType_)) {
            if (RequireValue(context, value, sideNode)) {
                Report(context, DiagnosticCode::ConditionNotBool, sideNode,
                       "The operand of '&&' and '||' must be 'bool'.",
                       "Conditions are never made from other types silently.",
                       "Compare it, as in 'x != null'.");
            }

            fine = false;
        }
    }

    if (!fine) {
        return ExpressionValue{};
    }

    ExpressionValue value;
    value.kind = K::Typed;
    value.type = boolType_;
    static_cast<void>(FoldBinary(context, node, op, left, right, boolType_, value));
    return value;
}

ExpressionValue TypeChecker::CheckCoalesce(const CheckContext& context, uint32 node,
                                           uint32 expected) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CheckCoalesceValue(context, node, expected);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CheckCoalesceValue(const CheckContext& context, uint32 node,
                                                uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    const uint32 leftNode = tree.Child(node, 0);
    const uint32 rightNode = tree.Child(node, 1);
    ExpressionValue result;
    auto typed = [&](uint32 type) noexcept {
        ExpressionValue value;
        value.kind = type == NoType ? K::Error : K::Typed;
        value.type = type;
        return value;
    };

    const ExpressionValue left = Check(context, leftNode, MakeOptional(expected));
    if (left.kind == K::Error) {
        static_cast<void>(Check(context, rightNode, UnresolvedType));
        return result;
    }

    if (left.kind != K::Typed || !IsOptional(left.type)) {
        if (RequireValue(context, left, leftNode)) {
            Report(context, DiagnosticCode::NotOptional, leftNode,
                   "The left side of '?"
                   "?' must have a type with '?'.",
                   "'?"
                   "?' chooses the right side when the left side is null.",
                   "Remove '?? ...'.");
        }

        static_cast<void>(Check(context, rightNode, UnresolvedType));
        return result;
    }

    const uint32 inner = NonOptional(left.type);
    const ExpressionValue right = Check(context, rightNode, inner);
    if (right.kind == K::Null) {
        Record(context, rightNode, left.type); // `x ?? null`은 `T?`다
        return typed(left.type);
    }

    if (right.kind == K::Typed && IsOptional(right.type)) {
        if (!IsAssignable(right.type, left.type)) {
            ReportMismatch(context, rightNode, right.type, left.type);
            return result;
        }

        return typed(left.type);
    }

    return typed(Coerce(context, right, inner, rightNode) == NoType ? NoType : inner);
}

ExpressionValue TypeChecker::CombineBinary(const CheckContext& context, uint32 node,
                                           const ExpressionValue& left,
                                           const ExpressionValue& right) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CombineBinaryValue(context, node, left, right);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CombineBinaryValue(const CheckContext& context, uint32 node,
                                                const ExpressionValue& left,
                                                const ExpressionValue& right) noexcept {
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

    // 두 피연산자의 타입을 맞춘다(한쪽이 상수·null이면 다른 쪽 타입으로).
    if (left.kind == K::Error || right.kind == K::Error) {
        return result;
    }

    if (!RequireValue(context, left, leftNode) || !RequireValue(context, right, rightNode)) {
        return result;
    }

    // `null`과의 비교: 다른 쪽이 `T?` 값이어야 한다(3.10절 "선택 타입 연산", S0407).
    if ((op == "==" || op == "!=") && (left.kind == K::Null || right.kind == K::Null)) {
        const ExpressionValue& other = left.kind == K::Null ? right : left;
        if (other.kind == K::Typed && IsOptional(other.type)) {
            Record(context, left.kind == K::Null ? leftNode : rightNode, other.type);
            return typed(boolType_);
        }

        Report(context, DiagnosticCode::NotComparable, node,
               "Only a value whose type has '?' can be compared with 'null'.",
               "A value of a type without '?' is never null.", "Remove the comparison.");
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
            Report(context, DiagnosticCode::InvalidOperator, node, "'%' is only for integers.", "",
                   "Use integers.");
            return result;
        }

        ExpressionValue folded;
        if (integer) {
            folded.kind = K::UntypedInt;
            folded.constant = left.constant;
            bool fine = true;
            if (op == "+") {
                fine =
                    AddConstant(folded.constant, right.constant.negative, right.constant.magnitude);
            } else if (op == "-") {
                fine = AddConstant(folded.constant,
                                   !right.constant.negative && right.constant.magnitude != 0,
                                   right.constant.magnitude);
            } else if (op == "*") {
                fine = MultiplyConstant(folded.constant, right.constant.negative,
                                        right.constant.magnitude);
            } else {
                if (ReportZeroDivisor(context, right, rightNode)) {
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

    // 한쪽만 상수·null이면 다른 쪽 타입으로 맞춘다. 다른 쪽이 숫자·벡터가 아니면 상수는 맞추지 않고
    // 기본 타입(int·float)으로 보아 연산자 규칙으로 판정한다(3.10절 "연산자"). `T?` 숫자에는 `T`로
    // 맞춘다(산술은 S0406, `==`는 `T?`와 `T`).
    auto untyped = [](const ExpressionValue& value) noexcept {
        return value.kind == K::UntypedInt || value.kind == K::UntypedFloat;
    };
    auto placeFor = [&](uint32 typedSide) noexcept {
        const uint32 bare = NonOptional(typedSide);
        return IsVector(bare) ? floatType_ : bare;
    };
    auto adapts = [&](uint32 typedSide) noexcept {
        const uint32 bare = NonOptional(typedSide);
        return Numeric(bare).kind != NumericKind::None || IsVector(bare);
    };
    uint32 leftType = NoType;
    uint32 rightType = NoType;
    if (left.kind == K::Typed) {
        leftType = left.type;
        rightType = right.kind == K::Typed ? right.type
                    : untyped(right) && !adapts(leftType)
                        ? Materialize(context, right, rightNode)
                        : Coerce(context, right, placeFor(leftType), rightNode);
    } else if (right.kind == K::Typed) {
        rightType = right.type;
        leftType = untyped(left) && !adapts(rightType)
                       ? Materialize(context, left, leftNode)
                       : Coerce(context, left, placeFor(rightType), leftNode);
    } else if (untyped(left) && untyped(right)) {
        // 상수끼리의 비교: 둘 다 정수면 int, 하나라도 실수면 float로 비교한다.
        const uint32 common =
            left.kind == K::UntypedFloat || right.kind == K::UntypedFloat ? floatType_ : intType_;
        leftType = Coerce(context, left, common, leftNode);
        rightType = Coerce(context, right, common, rightNode);
    } else {
        leftType = Materialize(context, left, leftNode);
        rightType = Materialize(context, right, rightNode);
    }

    if (op == "==" || op == "!=") {
        if (leftType == NoType || rightType == NoType) {
            return result;
        }

        const bool related = IsAssignable(leftType, rightType) || IsAssignable(rightType, leftType);
        if (!related || !IsEquatable(NonOptional(leftType))) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("Values of type '");
            AppendDisplayType(message, leftType);
            if (rightType != leftType) {
                message.Append("' and '");
                AppendDisplayType(message, rightType);
            }

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

        ExpressionValue value = typed(boolType_);
        if (!FoldBinary(context, node, op, left, right, NonOptional(leftType), value)) {
            return result;
        }

        return value;
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
        AppendDisplayType(message, leftType);
        message.Append("' and '");
        AppendDisplayType(message, rightType);
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
            ExpressionValue value = typed(boolType_);
            if (!FoldBinary(context, node, op, left, right, leftType, value)) {
                return result;
            }

            return value;
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
            Report(context, DiagnosticCode::InvalidOperator, node, "'%' is only for integers.", "",
                   "Use integers.");
            return result;
        }

        if (op == "%" && ReportZeroDivisor(context, right, rightNode)) {
            return result;
        }

        ExpressionValue value = typed(leftType);
        if (!FoldBinary(context, node, op, left, right, leftType, value)) {
            return result;
        }

        return value;
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
    const bool optional = IsOptional(leftType) || IsOptional(rightType);
    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    message.Append("'").Append(op).Append("' is not defined for '");
    AppendDisplayType(message, leftType);
    message.Append("' and '");
    AppendDisplayType(message, rightType);
    message.Append("'.");
    if (binder_.Succeeded(message)) {
        Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
               strings    ? StringView("Strings are joined with interpolation, not '+'.")
               : optional ? StringView("A value whose type has '?' may be null.")
                          : StringView(""),
               strings    ? StringView("Write \"${a}${b}\".")
               : optional ? StringView("Use '?? value' or 'if (let x = ...)' first.")
                          : StringView("Use operands of a type that has the operator."));
    }

    return result;
}

ExpressionValue TypeChecker::CheckUnary(const CheckContext& context, uint32 node,
                                        uint32 expected) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CheckUnaryValue(context, node, expected);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CheckUnaryValue(const CheckContext& context, uint32 node,
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
        if (operand.constant.kind == ConstantValue::Kind::Bool) {
            result.constant = operand.constant;
            result.constant.boolean = !operand.constant.boolean;
        } else if (operand.constant.kind == ConstantValue::Kind::Invalid) {
            result.constant.kind = ConstantValue::Kind::Invalid;
        }

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
        AppendDisplayType(message, type);
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::InvalidOperator, node, message.View(), "",
                   "Use a number or a vector.");
        }

        return result;
    }

    result.kind = K::Typed;
    result.type = type;
    // 정해진 상수의 부호 바꾸기(3.10절 "정해진 상수 접기").
    if (operand.constant.kind == ConstantValue::Kind::Invalid) {
        result.constant.kind = ConstantValue::Kind::Invalid;
    } else if (operand.constant.kind == ConstantValue::Kind::Float) {
        result.constant = operand.constant;
        result.constant.real = -operand.constant.real;
    } else if (operand.constant.kind == ConstantValue::Kind::Integer) {
        result.constant = operand.constant;
        result.constant.negative = !operand.constant.negative && operand.constant.magnitude != 0;
        if (!ConstantFits(result.constant, true, type)) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.Append("The constant expression overflows '");
            AppendDisplayType(message, type);
            message.Append("'.");
            if (binder_.Succeeded(message)) {
                Report(context, DiagnosticCode::ConstantOverflow, node, message.View(),
                       "A constant expression of a typed constant must fit in its type.",
                       "Use a value in range or a wider type.");
            }

            return ExpressionValue{};
        }
    }

    return result;
}

ExpressionValue TypeChecker::CheckIndex(const CheckContext& context, uint32 node) noexcept {
    // 재귀 경로의 frame에는 받는 값만 둔다(`a[0][0]...` 연쇄 — CheckBinary와 같은 이유).
    const ExpressionValue object =
        Check(context, compilation_.trees_[context.module].Child(node, 0), NoType);
    return IndexValue(context, node, object);
}

ExpressionValue TypeChecker::IndexValue(const CheckContext& context, uint32 node,
                                        const ExpressionValue& object) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = JudgeIndex(context, node, object);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::JudgeIndex(const CheckContext& context, uint32 node,
                                        const ExpressionValue& object) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    if (object.kind == K::Error || !RequireValue(context, object, tree.Child(node, 0))) {
        static_cast<void>(Check(context, tree.Child(node, 1), UnresolvedType));
        return result;
    }

    const uint32 type = Materialize(context, object, tree.Child(node, 0));
    if (IsOptional(type)) {
        Report(context, DiagnosticCode::MayBeNull, tree.Child(node, 0),
               "The value may be null, so it cannot be indexed.",
               "A value whose type has '?' must be checked first.",
               "Use 'if (let x = value) { ... }'.");
        static_cast<void>(Check(context, tree.Child(node, 1), UnresolvedType));
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
        AppendDisplayType(message, type);
        message.Append("' cannot be indexed.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::InvalidOperator, node, message.View(),
                   "Only arrays and maps are indexed.", "Use an array or a map.");
        }

        static_cast<void>(Check(context, tree.Child(node, 1), UnresolvedType));
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
    bool failed = false;
    if (tree.Node(node).kind == SyntaxKind::String) {
        for (const uint32 part : tree.Children(node)) {
            if (tree.Node(part).kind != SyntaxKind::Interpolation) {
                continue;
            }

            interpolated = true;
            const uint32 inner = tree.Child(part, 0);
            const ExpressionValue value = Check(context, inner, NoType);
            if (value.kind == K::Error || !RequireValue(context, value, inner)) {
                failed = true;
                continue;
            }

            const uint32 type = Materialize(context, value, inner);
            failed = failed || type == NoType;
            if (type != NoType && !IsInterpolatable(type)) {
                failed = true;
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.Append("A value of type '");
                AppendDisplayType(message, type);
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

    // 보간 식에 진단이 있으면 문자열도 해석하지 못한 것이다(연쇄 방지).
    if (failed) {
        return ExpressionValue{};
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
    // 재귀 경로의 frame에는 피연산자만 둔다(Check 참고).
    const ExpressionValue operand =
        Check(context, compilation_.trees_[context.module].Child(node, 0), NoType);
    return CastOf(context, node, operand);
}

ExpressionValue TypeChecker::CastOf(const CheckContext& context, uint32 node,
                                    const ExpressionValue& operand) noexcept {
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CastOfValue(context, node, operand);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CastOfValue(const CheckContext& context, uint32 node,
                                         const ExpressionValue& operand) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    const uint32 operandNode = tree.Child(node, 0);
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
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CheckAwaitTryValue(context, node);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CheckAwaitTryValue(const CheckContext& context, uint32 node) noexcept {
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
    // 판정이 끝난 값의 타입을 기록한다(재귀 경로 밖 — Check 참고).
    const ExpressionValue value = CheckCollectionValue(context, node, expected);
    if (value.kind == ExpressionValue::Kind::Typed) {
        Record(context, node, value.type);
    }

    return value;
}

ExpressionValue TypeChecker::CheckCollectionValue(const CheckContext& context, uint32 node,
                                                  uint32 expected) noexcept {
    using K = ExpressionValue::Kind;
    const SyntaxTree& tree = compilation_.trees_[context.module];
    ExpressionValue result;
    const Span<const uint32> elements = tree.Children(node);
    const bool entries = !elements.IsEmpty() && tree.Node(elements[0]).kind == SyntaxKind::MapEntry;
    // 안의 원소를 기대 타입 모름으로 검사한다(이름 검사만 — 연쇄 방지).
    auto checkElements = [&]() noexcept {
        for (const uint32 element : elements) {
            if (tree.Node(element).kind == SyntaxKind::MapEntry) {
                static_cast<void>(Check(context, tree.Child(element, 0), UnresolvedType));
                static_cast<void>(Check(context, tree.Child(element, 1), UnresolvedType));
            } else {
                static_cast<void>(Check(context, element, UnresolvedType));
            }
        }
    };
    if (expected == UnresolvedType) {
        checkElements();
        return result;
    }

    const uint32 target = NonOptional(expected);
    const bool isArray = IsLanguage(target, "Array");
    const bool isSet = IsLanguage(target, "Set");
    const bool isMap = IsLanguage(target, "Map");
    if (expected != NoType && !isArray && !isSet && !isMap) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("A collection literal cannot be used as '");
        AppendDisplayType(message, expected);
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::TypeMismatch, node, message.View(),
                   "'[...]' makes an Array, a Set or a Map.", "Use a value of the expected type.");
        }

        checkElements();
        return result;
    }
    if (expected != NoType && (isArray || isSet || isMap)) {
        if (entries != isMap && !elements.IsEmpty()) {
            Report(context, DiagnosticCode::TypeMismatch, node,
                   isMap ? StringView("A map literal is written with 'key: value' entries.")
                         : StringView("An array or set literal has no 'key: value' entries."),
                   "", "Write the literal for the expected type.");
            return result;
        }

        bool failed = false;
        auto checkElement = [&](uint32 at, uint32 type) noexcept {
            const ExpressionValue value = Check(context, at, type);
            return value.kind != K::Error && Coerce(context, value, type, at) != NoType;
        };
        for (const uint32 element : elements) {
            if (isMap) {
                if (tree.Node(element).kind != SyntaxKind::MapEntry) {
                    continue;
                }

                const uint32 keyNode = tree.Child(element, 0);
                const uint32 valueNode = tree.Child(element, 1);
                failed = !checkElement(keyNode, Argument(target, 0)) || failed;
                failed = !checkElement(valueNode, Argument(target, 1)) || failed;
            } else {
                failed = !checkElement(element, Argument(target, 0)) || failed;
            }
        }

        // 원소에 진단이 있으면 리터럴도 해석하지 못한 것이다(연쇄 방지).
        if (failed) {
            return result;
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

        if (value.kind == K::Null) {
            Report(context, DiagnosticCode::NoExpectedType, element, "'null' needs a type here.",
                   "Nothing tells which type the value has.",
                   "Declare the array type; elements cannot be null (use 'Array<T>' without '?').");
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
        if (values[index].kind == K::Typed) {
            continue;
        }

        // 숫자가 아닌 원소 타입과 숫자 상수가 섞이면 원소 타입이 다른 것이다(S0418).
        if (Numeric(elementType).kind == NumericKind::None) {
            Report(context, DiagnosticCode::MixedCollectionTypes, elements[index],
                   "The elements of the array literal have different types.",
                   "Elements are not combined into a base type.",
                   "Declare the array type, as in 'var items: Array<Actor> = [a, b];'.");
            return result;
        }

        if (Coerce(context, values[index], elementType, elements[index]) == NoType) {
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

bool TypeChecker::FoldBinary(const CheckContext& context, uint32 node, StringView op,
                             const ExpressionValue& left, const ExpressionValue& right,
                             uint32 operandType, ExpressionValue& result) noexcept {
    using CK = ConstantValue::Kind;
    const ConstantValue& a = left.constant;
    const ConstantValue& b = right.constant;
    if (a.kind == CK::Invalid || b.kind == CK::Invalid) {
        result.constant.kind = CK::Invalid; // 앞선 진단의 상수 — 값 없이 더 알리지 않는다
        return true;
    }

    if (a.kind == CK::None || b.kind == CK::None) {
        return true;
    }

    auto setBool = [&](bool value) noexcept {
        result.constant = ConstantValue{};
        result.constant.kind = CK::Bool;
        result.constant.boolean = value;
    };
    const bool equality = op == "==" || op == "!=";
    const bool ordering = op == "<" || op == "<=" || op == ">" || op == ">=";
    auto fromOrder = [&](int order) noexcept {
        if (op == "==") {
            return order == 0;
        }

        if (op == "!=") {
            return order != 0;
        }

        if (op == "<") {
            return order < 0;
        }

        if (op == "<=") {
            return order <= 0;
        }

        return op == ">" ? order > 0 : order >= 0;
    };
    if (a.kind == CK::Bool && b.kind == CK::Bool) {
        if (op == "&&") {
            setBool(a.boolean && b.boolean);
        } else if (op == "||") {
            setBool(a.boolean || b.boolean);
        } else if (equality) {
            setBool(fromOrder(a.boolean == b.boolean ? 0 : 1));
        }

        return true;
    }

    if (a.kind == CK::Enum && b.kind == CK::Enum) {
        if (equality) {
            setBool(fromOrder(a.symbol == b.symbol ? 0 : 1));
        }

        return true; // enum 순서 비교는 선언 순서 — 접지 않는다
    }

    const NumericInfo numeric = Numeric(operandType);
    if (numeric.kind == NumericKind::None || a.kind == CK::String || b.kind == CK::String) {
        return true; // 문자열·그 밖의 타입은 접지 않는다
    }

    if (numeric.kind == NumericKind::Float) {
        const double x = ToReal(a);
        const double y = ToReal(b);
        if (equality || ordering) {
            setBool(fromOrder(x < y ? -1 : (x > y ? 1 : 0)));
            return true;
        }

        double value = op == "+" ? x + y : op == "-" ? x - y : op == "*" ? x * y : x / y;
        // float32는 최근접 짝수로 반올림하며 반올림이 무한대가 되는 값은 유한하지 않다.
        const bool finite =
            IsFinite(value) &&
            (numeric.bits == 64 || (value < Float32RoundingLimit && value > -Float32RoundingLimit));
        if (finite && numeric.bits == 32) {
            value = static_cast<double>(static_cast<float>(value));
        }

        if (!finite) {
            Report(context, DiagnosticCode::NonFiniteConstant, node,
                   "The constant expression is not a finite number.",
                   "Constant arithmetic that makes NaN or infinity is an error.",
                   "Change the values.");
            return false;
        }

        result.constant = ConstantValue{};
        result.constant.kind = CK::Float;
        result.constant.real = value;
        return true;
    }

    if (a.kind != CK::Integer || b.kind != CK::Integer) {
        return true;
    }

    if (equality || ordering) {
        int order = 0;
        if (a.negative != b.negative) {
            order = a.negative ? -1 : 1;
        } else if (a.magnitude != b.magnitude) {
            order = (a.magnitude < b.magnitude) != a.negative ? -1 : 1;
        }

        setBool(fromOrder(order));
        return true;
    }

    ConstantValue value = a;
    bool fine = true;
    if (op == "+") {
        fine = AddConstant(value, b.negative, b.magnitude);
    } else if (op == "-") {
        fine = AddConstant(value, !b.negative && b.magnitude != 0, b.magnitude);
    } else if (op == "*") {
        fine = MultiplyConstant(value, b.negative, b.magnitude);
    } else if (op == "%") {
        value = RemainderConstant(a, b); // 0은 부르는 쪽이 막았다(S0504)
    } else {
        return true;
    }

    if (!fine || !ConstantFits(value, true, operandType)) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.Append("The constant expression overflows '");
        AppendDisplayType(message, operandType);
        message.Append("'.");
        if (binder_.Succeeded(message)) {
            Report(context, DiagnosticCode::ConstantOverflow, node, message.View(),
                   "A constant expression of a typed constant must fit in its type.",
                   "Use a value in range or a wider type.");
        }

        return false;
    }

    result.constant = value;
    return true;
}

} // namespace rocks::sparks::detail
