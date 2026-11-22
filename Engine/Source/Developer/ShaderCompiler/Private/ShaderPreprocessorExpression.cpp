#include "ShaderPreprocessorInternal.h"

#include <Base/NumericLimits.h>

// #if·#elif 식 계산이다(C 표준 6.10.1). defined를 먼저 바꾸고 매크로를 펼친 뒤 남은 식별자는
// true·false(C++ 규칙)는 1·0, 그 밖은 0이다. 컴파일러 내장 이름(__로 시작)은 펼치기 전후 모두
// 오류다(SC3). 값은 64-bit 정수(intmax_t·uintmax_t)이고 부호 없는 피연산자가 섞이면 부호 없이
// 계산한다. 계산하지 않는 쪽(&&·||·?:의 건너뛴 쪽)의 0 나눗셈은 오류가 아니다.
namespace rocks::shader::preprocess_detail {

namespace preprocess_expression_detail {

// 괄호·단항 연산자 중첩 상한이다(재귀 하강 파서의 스택 보호).
constexpr uint32 MaxConditionNesting = 256;
// 비트 이동 폭의 상한(64-bit 값)이다.
constexpr uint64 ValueBits = 64;

constexpr StringView OneText("1");
constexpr StringView ZeroText("0");

struct Value {
    uint64 bits = 0;
    bool isUnsigned = false;

    int64 Signed() const noexcept {
        return static_cast<int64>(bits);
    }

    bool IsTrue() const noexcept {
        return bits != 0;
    }
};

Value SignedValue(int64 value) noexcept {
    Value result;
    result.bits = static_cast<uint64>(value);
    return result;
}

Value BoolValue(bool value) noexcept {
    return SignedValue(value ? 1 : 0);
}

bool IsDigitOf(char value, uint32 base, uint32& digit) noexcept {
    if (value >= '0' && value <= '9') {
        digit = static_cast<uint32>(value - '0');
    } else if (value >= 'a' && value <= 'f') {
        digit = static_cast<uint32>(value - 'a') + 10u;
    } else if (value >= 'A' && value <= 'F') {
        digit = static_cast<uint32>(value - 'A') + 10u;
    } else {
        return false;
    }

    return digit < base;
}

// 정수 리터럴(10·16·8·2진, 접미사 u·l·ll 조합)을 읽는다.
bool ParseInteger(StringView text, Value& out) noexcept {
    const char* data = text.Data();
    const usize size = text.ByteCount();
    usize cursor = 0;
    uint32 base = 10;
    if (size >= 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) {
        base = 16;
        cursor = 2;
    } else if (size >= 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B')) {
        base = 2;
        cursor = 2;
    } else if (size >= 1 && data[0] == '0') {
        base = 8;
    }

    const usize digitsStart = cursor;
    uint64 value = 0;
    uint32 digit = 0;
    while (cursor < size && IsDigitOf(data[cursor], base, digit)) {
        if (value > (NumericLimits<uint64>::Max() - digit) / base) {
            return false;
        }

        value = value * base + digit;
        ++cursor;
    }

    if (cursor == digitsStart && base != 8) {
        return false;
    }

    bool isUnsigned = false;
    usize longs = 0;
    while (cursor < size) {
        const char suffix = data[cursor];
        if ((suffix == 'u' || suffix == 'U') && !isUnsigned) {
            isUnsigned = true;
            ++cursor;
        } else if ((suffix == 'l' || suffix == 'L') && longs == 0) {
            longs = cursor + 1 < size && data[cursor + 1] == suffix ? 2 : 1;
            cursor += longs;
        } else {
            return false;
        }
    }

    out.bits = value;
    out.isUnsigned = isUnsigned || value > static_cast<uint64>(NumericLimits<int64>::Max());
    return true;
}

class Parser final {
public:
    Parser(Context& context, Span<const Token> tokens, uint32 line) noexcept
        : context_(context), tokens_(tokens), line_(line) {
    }

    bool Evaluate(Value& out) noexcept {
        if (!Conditional(true, out)) {
            return false;
        }

        if (cursor_ != tokens_.Size()) {
            return Fail("unexpected token in preprocessor expression", tokens_[cursor_].text);
        }

        return true;
    }

private:
    bool Fail(StringView message, StringView detail = StringView()) noexcept {
        return context_.Fail(ShaderSourceError::InvalidExpression, line_, message, detail);
    }

    bool At(StringView punctuator) const noexcept {
        return cursor_ < tokens_.Size() && IsPunctuator(tokens_[cursor_], punctuator);
    }

    bool Enter() noexcept {
        if (++nesting_ > MaxConditionNesting) {
            return Fail("preprocessor expression is nested too deeply");
        }

        return true;
    }

    bool Conditional(bool live, Value& out) noexcept {
        if (!Enter() || !Binary(1, live, out)) {
            return false;
        }

        if (At("?")) {
            ++cursor_;
            const bool condition = out.IsTrue();
            Value whenTrue;
            Value whenFalse;
            if (!Conditional(live && condition, whenTrue)) {
                return false;
            }

            if (!At(":")) {
                return Fail("expected ':' in preprocessor expression");
            }

            ++cursor_;
            if (!Conditional(live && !condition, whenFalse)) {
                return false;
            }

            out = condition ? whenTrue : whenFalse;
            out.isUnsigned = whenTrue.isUnsigned || whenFalse.isUnsigned;
        }

        --nesting_;
        return true;
    }

    // 이항 연산자 우선순위(클수록 먼저)다. 아니면 0.
    uint32 Precedence() const noexcept {
        if (cursor_ >= tokens_.Size() || tokens_[cursor_].kind != TokenKind::Punctuator) {
            return 0;
        }

        const StringView text = tokens_[cursor_].text;
        if (text == StringView("||")) {
            return 1;
        }

        if (text == StringView("&&")) {
            return 2;
        }

        if (text == StringView("|")) {
            return 3;
        }

        if (text == StringView("^")) {
            return 4;
        }

        if (text == StringView("&")) {
            return 5;
        }

        if (text == StringView("==") || text == StringView("!=")) {
            return 6;
        }

        if (text == StringView("<") || text == StringView(">") || text == StringView("<=") ||
            text == StringView(">=")) {
            return 7;
        }

        if (text == StringView("<<") || text == StringView(">>")) {
            return 8;
        }

        if (text == StringView("+") || text == StringView("-")) {
            return 9;
        }

        if (text == StringView("*") || text == StringView("/") || text == StringView("%")) {
            return 10;
        }

        return 0;
    }

    bool Binary(uint32 minimum, bool live, Value& out) noexcept {
        if (!Unary(live, out)) {
            return false;
        }

        while (true) {
            const uint32 precedence = Precedence();
            if (precedence == 0 || precedence < minimum) {
                return true;
            }

            const StringView op = tokens_[cursor_].text;
            ++cursor_;
            // &&·||는 왼쪽 값에 따라 오른쪽을 계산하지 않는다.
            bool rightLive = live;
            if (op == StringView("&&")) {
                rightLive = live && out.IsTrue();
            } else if (op == StringView("||")) {
                rightLive = live && !out.IsTrue();
            }

            Value right;
            if (!Binary(precedence + 1, rightLive, right)) {
                return false;
            }

            if (!Apply(op, live && rightLive, out, right)) {
                return false;
            }
        }
    }

    bool Apply(StringView op, bool live, Value& left, Value right) noexcept {
        const bool isUnsigned = left.isUnsigned || right.isUnsigned;
        if (op == StringView("||")) {
            left = BoolValue(left.IsTrue() || right.IsTrue());
            return true;
        }

        if (op == StringView("&&")) {
            left = BoolValue(left.IsTrue() && right.IsTrue());
            return true;
        }

        if (op == StringView("<<") || op == StringView(">>")) {
            if ((!right.isUnsigned && right.Signed() < 0) || right.bits >= ValueBits) {
                if (live) {
                    return Fail("shift count is negative or too large in preprocessor expression");
                }

                left.bits = 0;
                return true;
            }

            if (op == StringView("<<")) {
                left.bits <<= right.bits;
            } else if (left.isUnsigned) {
                left.bits >>= right.bits;
            } else {
                left.bits = static_cast<uint64>(left.Signed() >> right.bits);
            }

            return true;
        }

        if (op == StringView("==") || op == StringView("!=")) {
            const bool equal = left.bits == right.bits;
            left = BoolValue(op == StringView("==") ? equal : !equal);
            return true;
        }

        if (op == StringView("<") || op == StringView(">") || op == StringView("<=") ||
            op == StringView(">=")) {
            const bool less = isUnsigned ? left.bits < right.bits : left.Signed() < right.Signed();
            const bool greater =
                isUnsigned ? left.bits > right.bits : left.Signed() > right.Signed();
            bool result = false;
            if (op == StringView("<")) {
                result = less;
            } else if (op == StringView(">")) {
                result = greater;
            } else if (op == StringView("<=")) {
                result = !greater;
            } else {
                result = !less;
            }

            left = BoolValue(result);
            return true;
        }

        Value result;
        result.isUnsigned = isUnsigned;
        if (op == StringView("|")) {
            result.bits = left.bits | right.bits;
        } else if (op == StringView("^")) {
            result.bits = left.bits ^ right.bits;
        } else if (op == StringView("&")) {
            result.bits = left.bits & right.bits;
        } else if (op == StringView("+")) {
            result.bits = left.bits + right.bits;
        } else if (op == StringView("-")) {
            result.bits = left.bits - right.bits;
        } else if (op == StringView("*")) {
            result.bits = left.bits * right.bits;
        } else {
            if (right.bits == 0) {
                if (live) {
                    return Fail("division by zero in preprocessor expression");
                }

                left = result;
                return true;
            }

            const bool divide = op == StringView("/");
            if (isUnsigned) {
                result.bits = divide ? left.bits / right.bits : left.bits % right.bits;
            } else if (left.Signed() == NumericLimits<int64>::Min() && right.Signed() == -1) {
                // 넘침은 2의 보수로 감는다(하드웨어 나눗셈 예외를 피한다).
                result.bits = divide ? left.bits : 0;
            } else {
                result.bits = static_cast<uint64>(divide ? left.Signed() / right.Signed()
                                                         : left.Signed() % right.Signed());
            }
        }

        left = result;
        return true;
    }

    bool Unary(bool live, Value& out) noexcept {
        if (cursor_ >= tokens_.Size()) {
            return Fail("missing operand in preprocessor expression");
        }

        const Token& token = tokens_[cursor_];
        if (token.kind == TokenKind::Punctuator &&
            (token.text == StringView("+") || token.text == StringView("-") ||
             token.text == StringView("~") || token.text == StringView("!"))) {
            ++cursor_;
            if (!Enter() || !Unary(live, out)) {
                return false;
            }

            --nesting_;
            if (token.text == StringView("-")) {
                out.bits = 0 - out.bits;
            } else if (token.text == StringView("~")) {
                out.bits = ~out.bits;
            } else if (token.text == StringView("!")) {
                out = BoolValue(!out.IsTrue());
            }

            return true;
        }

        if (IsPunctuator(token, "(")) {
            ++cursor_;
            if (!Conditional(live, out)) {
                return false;
            }

            if (!At(")")) {
                return Fail("expected ')' in preprocessor expression");
            }

            ++cursor_;
            return true;
        }

        if (token.kind == TokenKind::Number) {
            if (!ParseInteger(token.text, out)) {
                return Fail("invalid integer in preprocessor expression", token.text);
            }

            ++cursor_;
            return true;
        }

        return Fail("unsupported token in preprocessor expression", token.text);
    }

    Context& context_;
    Span<const Token> tokens_;
    uint32 line_;
    usize cursor_ = 0;
    uint32 nesting_ = 0;
};

class ConditionCollector final : public TokenSink {
public:
    explicit ConditionCollector(Array<Token>& out) noexcept : out_(&out) {
    }

    bool Put(const Token& token) noexcept override {
        return static_cast<bool>(out_->TryPushBack(token));
    }

private:
    Array<Token>* out_;
};

bool BuiltinInCondition(Context& context, uint32 line, StringView name) noexcept {
    return context.Fail(ShaderSourceError::CompilerBuiltinInCondition, line,
                        "compiler built-in names cannot be used in preprocessor conditions; use "
                        "the ROCKS_* defines",
                        name);
}

} // namespace preprocess_expression_detail

bool EvaluateCondition(Context& context, Span<const Token> tokens, uint32 line,
                       bool& value) noexcept {
    using namespace preprocess_expression_detail;
    if (tokens.IsEmpty()) {
        return context.Fail(ShaderSourceError::InvalidExpression, line, "#if with no expression");
    }

    // defined를 펼치기 전에 1·0으로 바꾼다(피연산자는 펼치지 않는다).
    Array<Token> work(context.allocator);
    for (usize index = 0; index < tokens.Size(); ++index) {
        const Token& token = tokens[index];
        if (token.kind == TokenKind::Identifier && token.text == StringView("defined")) {
            usize operand = index + 1;
            const bool parenthesized =
                operand < tokens.Size() && IsPunctuator(tokens[operand], "(");
            if (parenthesized) {
                ++operand;
            }

            if (operand >= tokens.Size() || tokens[operand].kind != TokenKind::Identifier ||
                (parenthesized &&
                 (operand + 1 >= tokens.Size() || !IsPunctuator(tokens[operand + 1], ")")))) {
                return context.Fail(ShaderSourceError::InvalidExpression, line,
                                    "'defined' requires a macro name");
            }

            const StringView name = tokens[operand].text;
            if (IsCompilerBuiltinName(name)) {
                return BuiltinInCondition(context, line, name);
            }

            Token result = token;
            result.kind = TokenKind::Number;
            result.text = context.FindMacro(name) != nullptr ? OneText : ZeroText;
            if (!work.TryPushBack(result)) {
                return context.OutOfMemory(line);
            }

            index = parenthesized ? operand + 1 : operand;
            continue;
        }

        if (token.kind == TokenKind::Identifier && IsCompilerBuiltinName(token.text)) {
            return BuiltinInCondition(context, line, token.text);
        }

        if (!work.TryPushBack(token)) {
            return context.OutOfMemory(line);
        }
    }

    Array<Token> expanded(context.allocator);
    ConditionCollector collector(expanded);
    if (!ExpandTokens(context, work, collector, nullptr, 0)) {
        return false;
    }

    for (Token& token : expanded) {
        if (token.kind != TokenKind::Identifier) {
            continue;
        }

        if (token.text == StringView("defined")) {
            return context.Fail(ShaderSourceError::InvalidExpression, line,
                                "'defined' produced by macro expansion is not supported");
        }

        if (IsCompilerBuiltinName(token.text)) {
            return BuiltinInCondition(context, line, token.text);
        }

        const bool isTrue = token.text == StringView("true");
        token.kind = TokenKind::Number;
        token.text = isTrue ? OneText : ZeroText;
    }

    Parser parser(context, expanded.View(), line);
    Value result;
    if (!parser.Evaluate(result)) {
        return false;
    }

    value = result.IsTrue();
    return true;
}

} // namespace rocks::shader::preprocess_detail
