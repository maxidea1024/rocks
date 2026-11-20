#include "SyntaxTreeBuilder.h"

// 식의 구문 분석이다(3.6절 Expr 이하와 우선순위 문단). 우선순위(높은 것부터): `.` `?.` `()` `[]` →
// `as?` → 단항 `!` `-` → `*` `/` `%` → `+` `-` → `??`(오른쪽 결합) → 비교(결합 없음) → `==`
// `!=`(결합 없음) → `&&` → `||`.

namespace rocks::sparks::detail {

uint32 SyntaxTreeBuilder::ParseExpression() noexcept {
    Nesting nesting(*this);
    if (nesting.TooDeep()) {
        return MakeError();
    }

    return ParseOr();
}

uint32 SyntaxTreeBuilder::MakeBinary(uint32 op, uint32 left, uint32 right) noexcept {
    const usize mark = Mark();
    Push(left);
    Push(right);
    return Make(SyntaxKind::Binary, mark, NodeBegin(left), op);
}

// Or = And { "||" And }
uint32 SyntaxTreeBuilder::ParseOr() noexcept {
    uint32 left = ParseAnd();
    while (At(TokenKind::PipePipe)) {
        const uint32 op = Advance();
        left = MakeBinary(op, left, ParseAnd());
    }

    return left;
}

// And = Eq { "&&" Eq }
uint32 SyntaxTreeBuilder::ParseAnd() noexcept {
    uint32 left = ParseEquality();
    while (At(TokenKind::AmpersandAmpersand)) {
        const uint32 op = Advance();
        left = MakeBinary(op, left, ParseEquality());
    }

    return left;
}

void SyntaxTreeBuilder::ReportChainedComparison() noexcept {
    ReportInPlace(DiagnosticCode::ChainedComparison, CurrentSpan(),
                  "Comparisons cannot be chained.",
                  "Comparison and equality operators take exactly two operands; 'a < b < c' would "
                  "compare a bool with c.",
                  "Combine two comparisons with '&&', as in 'a < b && b < c'.");
}

// Eq = Cmp [ ( "==" | "!=" ) Cmp ]. 잇달아 쓰면 S0209를 한 번 내고 왼쪽 결합으로 이어 읽는다.
uint32 SyntaxTreeBuilder::ParseEquality() noexcept {
    uint32 left = ParseComparison();
    if (!IsEquality(Kind())) {
        return left;
    }

    uint32 op = Advance();
    left = MakeBinary(op, left, ParseComparison());
    if (IsEquality(Kind())) {
        ReportChainedComparison();
        while (IsEquality(Kind())) {
            op = Advance();
            left = MakeBinary(op, left, ParseComparison());
        }
    }

    return left;
}

// Cmp = Coal [ ( "<" | "<=" | ">" | ">=" ) Coal ]. 잇달아 쓰면 S0209다(Eq와 같다).
uint32 SyntaxTreeBuilder::ParseComparison() noexcept {
    uint32 left = ParseCoalesce();
    if (!IsComparison(Kind())) {
        return left;
    }

    uint32 op = Advance();
    left = MakeBinary(op, left, ParseCoalesce());
    if (IsComparison(Kind())) {
        ReportChainedComparison();
        while (IsComparison(Kind())) {
            op = Advance();
            left = MakeBinary(op, left, ParseCoalesce());
        }
    }

    return left;
}

// Coal = Add [ "??" Coal ]. `?? return`·`?? break`·`?? continue` 앞에서는 멈춘다(지역 선언이 Exit로
// 읽거나 S0213).
uint32 SyntaxTreeBuilder::ParseCoalesce() noexcept {
    const uint32 left = ParseAdditive();
    if (!At(TokenKind::QuestionQuestion) || IsExitKeyword(PeekKind(1))) {
        return left;
    }

    Nesting nesting(*this);
    if (nesting.TooDeep()) {
        return left;
    }

    const uint32 op = Advance();
    return MakeBinary(op, left, ParseCoalesce());
}

// Add = Mul { ( "+" | "-" ) Mul }
uint32 SyntaxTreeBuilder::ParseAdditive() noexcept {
    uint32 left = ParseMultiplicative();
    while (At(TokenKind::Plus) || At(TokenKind::Minus)) {
        const uint32 op = Advance();
        left = MakeBinary(op, left, ParseMultiplicative());
    }

    return left;
}

// Mul = Unary { ( "*" | "/" | "%" ) Unary }
uint32 SyntaxTreeBuilder::ParseMultiplicative() noexcept {
    uint32 left = ParseUnary();
    while (At(TokenKind::Star) || At(TokenKind::Slash) || At(TokenKind::Percent)) {
        const uint32 op = Advance();
        left = MakeBinary(op, left, ParseUnary());
    }

    return left;
}

// Unary = { "!" | "-" } Postfix [ "as?" Path ]. `as?`가 단항 연산자보다 먼저 묶인다(`-x as? T`는
// `-(x as? T)`).
uint32 SyntaxTreeBuilder::ParseUnary() noexcept {
    if (At(TokenKind::Bang) || At(TokenKind::Minus)) {
        Nesting nesting(*this);
        if (nesting.TooDeep()) {
            return MakeError();
        }

        const uint32 begin = CurrentBegin();
        const uint32 op = Advance();
        const usize mark = Mark();
        Push(ParseUnary());
        return Make(SyntaxKind::Unary, mark, begin, op);
    }

    const uint32 operand = ParsePostfix();
    if (!At(TokenKind::AsQuestion)) {
        return operand;
    }

    const usize mark = Mark();
    Push(operand);
    Advance();
    Push(ParsePath("after 'as?'"));
    usize scan = SkipDocs(position_);
    bool tooDeep = false;
    const bool typeArguments =
        TokenAt(scan).kind == TokenKind::Less && ScanTypeArguments(scan, 0, tooDeep);
    if (typeArguments || At(TokenKind::Question)) {
        ReportInPlace(DiagnosticCode::ExpectedToken, CurrentSpan(),
                      "A downcast target is a plain type name without type arguments or '?'.",
                      "'as?' already gives null when the object is not of that type.",
                      "Remove the type arguments or '?'.");
        const NodeCheckpoint checkpoint = SaveNodes();
        if (typeArguments) {
            static_cast<void>(ParseTypeArguments());
        }

        Accept(TokenKind::Question);
        RestoreNodes(checkpoint);
    }

    return Make(SyntaxKind::CastTest, mark, NodeBegin(operand));
}

// Postfix = Primary { "." Ident [ TypeArgs ] | "?." Ident [ TypeArgs ] [ Args ] | Args | "[" Expr
// "]" }
uint32 SyntaxTreeBuilder::ParsePostfix() noexcept {
    uint32 expression = ParsePrimary();
    while (true) {
        const TokenKind kind = Kind();
        if (kind == TokenKind::Dot || kind == TokenKind::QuestionDot) {
            Advance();
            uint32 name = NoToken;
            if (At(TokenKind::Identifier)) {
                name = Advance();
            } else {
                const bool inSync = CanReport() && !AtLexicalError();
                ReportExpected(DiagnosticCode::ExpectedName, "a member name",
                               kind == TokenKind::Dot ? StringView("after '.'")
                                                      : StringView("after '?.'"),
                               IsKeyword(Kind()) ? StringView("Keywords cannot be used as names.")
                                                 : StringView(),
                               "Write the member name, as in 'door.Open()'.");
                // 같은 줄의 키워드는 이름 자리의 잘못으로 보고 소비한다(`x.if`가 if 문으로 다시
                // 읽히지 않게, MakeNameError와 같은 규칙).
                if (AtMistakenKeyword(inSync)) {
                    Advance();
                }
            }

            const usize mark = Mark();
            Push(expression);
            Push(name != NoToken && At(TokenKind::Less) && TypeArgumentsAhead()
                     ? ParseTypeArguments()
                     : NoNode);
            expression = Make(SyntaxKind::Member, mark, NodeBegin(expression), name,
                              kind == TokenKind::QuestionDot ? SyntaxFlags::NullConditional : 0);
            if (name == NoToken) {
                return expression;
            }

            continue;
        }

        if (kind == TokenKind::LeftParen) {
            const usize mark = Mark();
            Push(expression);
            Push(ParseArguments());
            expression = Make(SyntaxKind::Call, mark, NodeBegin(expression));
            continue;
        }

        if (kind == TokenKind::LeftBracket) {
            Advance();
            const usize mark = Mark();
            Push(expression);
            Push(ParseExpression());
            Expect(TokenKind::RightBracket, "to close the index");
            expression = Make(SyntaxKind::Index, mark, NodeBegin(expression));
            continue;
        }

        return expression;
    }
}

// Args = "(" [ Arg { "," Arg } ] ")", Arg = [ Ident ":" ] Expr. Argument List다(범위는 괄호까지).
// 이름 지정 인자를 쓸 수 있는 호출인지는 뒤 단계가 판정한다.
uint32 SyntaxTreeBuilder::ParseArguments() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    if (!At(TokenKind::RightParen)) {
        do {
            const uint32 argumentBegin = CurrentBegin();
            const usize argumentMark = Mark();
            if (At(TokenKind::Identifier) && PeekKind(1) == TokenKind::Colon) {
                Push(Leaf(SyntaxKind::Name, Advance()));
                Advance();
            } else {
                Push(NoNode);
            }

            Push(ParseExpression());
            Push(Make(SyntaxKind::Argument, argumentMark, argumentBegin));
        } while (Accept(TokenKind::Comma));
    }

    Expect(TokenKind::RightParen, "to close the arguments");
    return Make(SyntaxKind::List, mark, begin);
}

// Primary = Literal | Ident [ TypeArgs ] | "this" | "(" Expr ")" | "[" [ Elem { "," Elem } ] "]"
uint32 SyntaxTreeBuilder::ParsePrimary() noexcept {
    switch (Kind()) {
    case TokenKind::IntegerLiteral:
        return Leaf(SyntaxKind::IntegerLiteral, Advance());
    case TokenKind::FloatLiteral:
        return Leaf(SyntaxKind::FloatLiteral, Advance());
    case TokenKind::True:
    case TokenKind::False:
        return Leaf(SyntaxKind::BoolLiteral, Advance());
    case TokenKind::Null:
        return Leaf(SyntaxKind::NullLiteral, Advance());
    case TokenKind::VerbatimString:
        return Leaf(SyntaxKind::VerbatimString, Advance());
    case TokenKind::This:
        return Leaf(SyntaxKind::This, Advance());
    case TokenKind::Identifier: {
        const uint32 begin = CurrentBegin();
        const uint32 name = Advance();
        const usize mark = Mark();
        Push(At(TokenKind::Less) && TypeArgumentsAhead() ? ParseTypeArguments() : NoNode);
        return Make(SyntaxKind::Identifier, mark, begin, name);
    }
    case TokenKind::StringBegin:
        return ParseString();
    case TokenKind::LeftParen: {
        const uint32 begin = CurrentBegin();
        Advance();
        const usize mark = Mark();
        Push(ParseExpression());
        Expect(TokenKind::RightParen, "to close the parentheses");
        return Make(SyntaxKind::Parenthesized, mark, begin);
    }
    case TokenKind::LeftBracket:
        return ParseCollection();
    case TokenKind::Try:
    case TokenKind::Await:
        if (At(TokenKind::Try) && lastKind_ == TokenKind::Await) {
            ReportInPlace(DiagnosticCode::MisplacedAwait, CurrentSpan(),
                          "'try' comes before 'await'.",
                          "A failing awaited call is written 'try await F()'.",
                          "Write 'try await' in that order.");
        } else {
            ReportInPlace(DiagnosticCode::MisplacedAwait, CurrentSpan(),
                          "'await' and 'try' can only start a statement.",
                          "They apply to a whole statement, a local variable's value, the right "
                          "side of '=' or a return value.",
                          "Store the value in a local variable first, as in 'let x = await F();'.");
        }

        return ParseTryAwait();
    case TokenKind::DotDotLess:
        ReportRangeOutsideFor();
        return MakeError();
    default:
        break;
    }

    ReportExpected(DiagnosticCode::ExpectedExpression, "an expression", StringView(), StringView(),
                   "Write a value or an expression.");
    return MakeError();
}

// 문자열 리터럴이다: StringBegin { StringText | InterpolationBegin Expr InterpolationEnd }
// StringEnd. 보간을 닫지 못하면 그 보간의 끝(또는 문자열 끝)까지 건너뛴다.
uint32 SyntaxTreeBuilder::ParseString() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    while (true) {
        const TokenKind kind = Kind();
        if (kind == TokenKind::StringText) {
            Push(Leaf(SyntaxKind::StringText, Advance()));
            continue;
        }

        if (kind == TokenKind::InterpolationBegin) {
            const uint32 interpolationBegin = CurrentBegin();
            Advance();
            const usize interpolationMark = Mark();
            Push(ParseExpression());
            if (!At(TokenKind::InterpolationEnd)) {
                Expect(TokenKind::InterpolationEnd, "to close the interpolation");
                usize depth = 0;
                while (true) {
                    const TokenKind skipped = PeekKind(0);
                    if (skipped == TokenKind::EndOfFile ||
                        (depth == 0 && (skipped == TokenKind::InterpolationEnd ||
                                        skipped == TokenKind::StringEnd))) {
                        break;
                    }

                    if (skipped == TokenKind::StringBegin ||
                        skipped == TokenKind::InterpolationBegin) {
                        ++depth;
                    } else if (skipped == TokenKind::StringEnd ||
                               skipped == TokenKind::InterpolationEnd) {
                        --depth;
                    }

                    SkipOne();
                }

                if (PeekKind(0) == TokenKind::InterpolationEnd) {
                    SkipOne();
                }
            } else {
                Advance();
            }

            Push(Make(SyntaxKind::Interpolation, interpolationMark, interpolationBegin));
            continue;
        }

        if (kind == TokenKind::Invalid) {
            // 닫히지 않은 문자열이다(어휘 분석이 S0108을 냈다). 진단 없이 복구 상태가 된다.
            panic_ = true;
            SkipOne();
            continue;
        }

        if (kind == TokenKind::StringEnd) {
            Advance();
        }

        break;
    }

    return Make(SyntaxKind::String, mark, begin);
}

// "[" [ Elem { "," Elem } ] "]", Elem = Expr [ ":" Expr ]. 값 원소와 `키: 값` 원소가 섞이면 S0218을
// 한 번 낸다.
uint32 SyntaxTreeBuilder::ParseCollection() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    bool sawValue = false;
    bool sawEntry = false;
    bool reported = false;
    if (!At(TokenKind::RightBracket)) {
        do {
            const uint32 key = ParseExpression();
            uint32 element = key;
            if (At(TokenKind::Colon)) {
                Advance();
                const usize entryMark = Mark();
                Push(key);
                Push(ParseExpression());
                element = Make(SyntaxKind::MapEntry, entryMark, NodeBegin(key));
                sawEntry = true;
            } else {
                sawValue = true;
            }

            Push(element);
            if (sawValue && sawEntry && !reported) {
                reported = true;
                ReportInPlace(DiagnosticCode::MixedCollectionLiteral, NodeSpan(element),
                              "A collection literal mixes values and 'key: value' entries.",
                              "An Array literal lists values and a Map literal lists 'key: value' "
                              "entries.",
                              "Use only values or only 'key: value' entries.");
            }
        } while (Accept(TokenKind::Comma));
    }

    Expect(TokenKind::RightBracket, "to close the collection literal");
    return Make(SyntaxKind::CollectionLiteral, mark, begin);
}

} // namespace rocks::sparks::detail
