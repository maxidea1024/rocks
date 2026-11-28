#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <UiFramework/Numeric/NumericExpression.h>

namespace rocks::ui {

namespace {

enum class TokenKind : uint8 {
    Number,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Caret,
    Open,
    Close,
    SquareRoot,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign
};

struct Token {
    TokenKind kind = TokenKind::Number;
    double value = 0;
};

constexpr StringView SquareRootWord = "sqrt";

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 글자를 토큰으로 나눈다. 토큰 수 상한을 넘으면 CapacityExceeded다.
Result<usize> Lex(StringView text, Token* tokens) noexcept {
    usize count = 0;
    const char* data = text.Data();
    const usize size = text.ByteCount();
    usize i = 0;
    const auto push = [&](TokenKind kind, double value = 0) noexcept {
        if (count == MaxNumericExpressionTokens) {
            return false;
        }

        tokens[count++] = {kind, value};
        return true;
    };
    while (i < size) {
        const char c = data[i];
        if (IsSpace(c)) {
            ++i;
            continue;
        }

        bool pushed = true;
        if (IsDigit(c) || c == '.') {
            // 숫자: 자릿수와 소수점 하나(UE FastDecimalFormat — 지수 표기는 받지 않는다).
            usize end = i;
            bool point = false;
            while (end < size && (IsDigit(data[end]) || (data[end] == '.' && !point))) {
                point = point || data[end] == '.';
                ++end;
            }

            auto parsed = TryParseDouble({data + i, end - i});
            if (!parsed || !math::IsFinite(parsed.Value())) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }

            pushed = push(TokenKind::Number, parsed.Value());
            i = end;
        } else if (size - i >= SquareRootWord.ByteCount() &&
                   StringView(data + i, SquareRootWord.ByteCount()) == SquareRootWord) {
            pushed = push(TokenKind::SquareRoot);
            i += SquareRootWord.ByteCount();
        } else {
            const bool assign = i + 1 < size && data[i + 1] == '=';
            TokenKind kind = TokenKind::Number;
            switch (c) {
            case '+':
                kind = assign ? TokenKind::PlusAssign : TokenKind::Plus;
                break;
            case '-':
                kind = assign ? TokenKind::MinusAssign : TokenKind::Minus;
                break;
            case '*':
                kind = assign ? TokenKind::StarAssign : TokenKind::Star;
                break;
            case '/':
                kind = assign ? TokenKind::SlashAssign : TokenKind::Slash;
                break;
            case '%':
                kind = TokenKind::Percent;
                break;
            case '^':
                kind = TokenKind::Caret;
                break;
            case '(':
                kind = TokenKind::Open;
                break;
            case ')':
                kind = TokenKind::Close;
                break;
            default:
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }

            const bool twoChars = assign && (c == '+' || c == '-' || c == '*' || c == '/');
            pushed = push(kind);
            i += twoChars ? 2 : 1;
        }

        if (!pushed) {
            return Result<usize>::Failure(ErrorCode::CapacityExceeded);
        }
    }

    return Result<usize>::Success(count);
}

// 재귀 하강 계산기다(우선순위: 단항 > ^ > * / % > + -).
class Parser {
public:
    Parser(const Token* tokens, usize count) noexcept : tokens_(tokens), count_(count) {
    }

    Result<double> Run() noexcept {
        auto value = Sum();
        if (value && position_ != count_) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }

        return value;
    }

private:
    bool At(TokenKind kind) const noexcept {
        return position_ < count_ && tokens_[position_].kind == kind;
    }

    Result<double> Sum() noexcept {
        auto left = Product();
        while (left && (At(TokenKind::Plus) || At(TokenKind::Minus))) {
            const bool add = At(TokenKind::Plus);
            ++position_;
            auto right = Product();
            if (!right) {
                return right;
            }

            left = Result<double>::Success(add ? left.Value() + right.Value()
                                               : left.Value() - right.Value());
        }

        return left;
    }

    Result<double> Product() noexcept {
        auto left = Power();
        while (left && (At(TokenKind::Star) || At(TokenKind::Slash) || At(TokenKind::Percent))) {
            const TokenKind op = tokens_[position_].kind;
            ++position_;
            auto right = Power();
            if (!right) {
                return right;
            }

            // UE: 0으로 나눔·0으로 나머지는 오류다.
            if (op != TokenKind::Star && right.Value() == 0) {
                return Result<double>::Failure(ErrorCode::InvalidArgument);
            }

            const double a = left.Value();
            const double b = right.Value();
            left = Result<double>::Success(op == TokenKind::Star    ? a * b
                                           : op == TokenKind::Slash ? a / b
                                                                    : math::Fmod(a, b));
        }

        return left;
    }

    // 오른쪽 결합이다(2^3^2 = 2^9).
    Result<double> Power() noexcept {
        auto base = Unary();
        if (!base || !At(TokenKind::Caret)) {
            return base;
        }

        ++position_;
        auto exponent = Power();
        if (!exponent) {
            return exponent;
        }

        return Result<double>::Success(platform::PlatformMath::Pow(base.Value(), exponent.Value()));
    }

    Result<double> Unary() noexcept {
        if (At(TokenKind::Plus) || At(TokenKind::Minus) || At(TokenKind::SquareRoot)) {
            const TokenKind op = tokens_[position_].kind;
            ++position_;
            auto operand = Unary();
            if (!operand) {
                return operand;
            }

            const double v = operand.Value();
            return Result<double>::Success(op == TokenKind::Plus    ? v
                                           : op == TokenKind::Minus ? -v
                                                                    : math::Sqrt(v));
        }

        return Primary();
    }

    Result<double> Primary() noexcept {
        if (At(TokenKind::Number)) {
            return Result<double>::Success(tokens_[position_++].value);
        }

        if (!At(TokenKind::Open)) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }

        if (depth_ == MaxNumericExpressionDepth) {
            return Result<double>::Failure(ErrorCode::CapacityExceeded);
        }

        ++position_;
        ++depth_;
        auto inner = Sum();
        --depth_;
        if (!inner) {
            return inner;
        }

        if (!At(TokenKind::Close)) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }

        ++position_;
        return inner;
    }

    const Token* tokens_;
    usize count_;
    usize position_ = 0;
    usize depth_ = 0;
};

} // namespace

Result<double> EvaluateNumericExpression(StringView text, double existing) noexcept {
    // 토큰 하나를 앞에 더 넣을 자리(상대 입력의 지금 값)를 둔다.
    Token tokens[MaxNumericExpressionTokens + 1];
    auto lexed = Lex(text, tokens + 1);
    if (!lexed) {
        return Result<double>::Failure(lexed.Error());
    }

    usize count = lexed.Value();
    if (count == 0) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    // 처음의 += -= *= /=는 "지금 값 op"로 바꾼다(UE Evaluate 233–266행).
    Token* first = tokens + 1;
    const TokenKind lead = first->kind;
    const bool assign = lead == TokenKind::PlusAssign || lead == TokenKind::MinusAssign ||
                        lead == TokenKind::StarAssign || lead == TokenKind::SlashAssign;
    if (assign) {
        if (!math::IsFinite(existing)) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }

        first->kind = lead == TokenKind::PlusAssign    ? TokenKind::Plus
                      : lead == TokenKind::MinusAssign ? TokenKind::Minus
                      : lead == TokenKind::StarAssign  ? TokenKind::Star
                                                       : TokenKind::Slash;
        tokens[0] = {TokenKind::Number, existing};
        first = tokens;
        ++count;
    }

    // 나머지 자리의 대입 토큰은 문법 오류다.
    for (usize i = 0; i < count; ++i) {
        const TokenKind kind = first[i].kind;
        if (kind == TokenKind::PlusAssign || kind == TokenKind::MinusAssign ||
            kind == TokenKind::StarAssign || kind == TokenKind::SlashAssign) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Parser parser(first, count);
    auto value = parser.Run();
    // 음수의 sqrt 같은 NaN은 잘못된 입력, 무한은 넘침이다.
    if (value && math::IsNaN(value.Value())) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    if (value && !math::IsFinite(value.Value())) {
        return Result<double>::Failure(ErrorCode::Overflow);
    }

    return value;
}

} // namespace rocks::ui
