#include "SyntaxTreeBuilder.h"

// 타입·타입 인자의 구문 분석과 식 안의 타입 인자 판정이다(3.6절 Type·TypeArgs와 그 앞 문단).

namespace rocks::sparks::detail {

// Type = Path [ TypeArgs ] [ "?" ]
uint32 SyntaxTreeBuilder::ParseType(StringView context) noexcept {
    Nesting nesting(*this);
    if (nesting.TooDeep()) {
        return MakeError();
    }

    if (!At(TokenKind::Identifier)) {
        const bool inSync = CanReport() && !AtLexicalError();
        ReportExpected(DiagnosticCode::ExpectedType, "a type", context,
                       IsKeyword(Kind()) ? StringView("Keywords cannot be used as type names.")
                                         : StringView(),
                       "Write a type such as 'int', 'Health?' or 'Array<Enemy>'.");
        return MakeNameError(inSync);
    }

    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    Push(ParsePath(context));
    Push(At(TokenKind::Less) ? ParseTypeArguments() : NoNode);
    const uint32 flags = Accept(TokenKind::Question) ? SyntaxFlags::Optional : 0;
    return Make(SyntaxKind::Type, mark, begin, NoToken, flags);
}

// TypeArgs = "<" Type { "," Type } ">"의 Type List다(범위는 꺾쇠까지). 닫는 자리의 `>=`는 `>`와
// `=`로 나눈다: 토큰을 `=`(범위는 `=` 한 글자)로 바꾸고 `>`는 이 List의 범위에만 남긴다.
uint32 SyntaxTreeBuilder::ParseTypeArguments() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    do {
        Push(ParseType("as a type argument"));
    } while (Accept(TokenKind::Comma));

    if (At(TokenKind::GreaterEqual)) {
        Token& token = tree_.tokens_[position_];
        token.kind = TokenKind::Equal;
        token.span.begin += 1;
        previousEnd_ = token.span.begin;
        lastKind_ = TokenKind::Greater;
        ++consumed_;
    } else {
        Expect(TokenKind::Greater, "to close the type arguments");
    }

    return Make(SyntaxKind::List, mark, begin);
}

bool SyntaxTreeBuilder::TypeArgumentsAhead() const noexcept {
    usize index = SkipDocs(position_);
    bool tooDeep = false;
    if (TokenAt(index).kind != TokenKind::Less || !ScanTypeArguments(index, 0, tooDeep)) {
        return false;
    }

    if (tooDeep) {
        return true;
    }

    const TokenKind next = TokenAt(index).kind;
    return next == TokenKind::LeftParen || next == TokenKind::Dot;
}

// index의 `<`부터 짝이 맞는 `>` 다음까지 타입 인자 모양인지 본다(진단·노드 없음). 성공하면 index는
// `>` 다음 토큰이다. `<`가 MaxSyntaxNesting 단계 넘게 중첩되면 tooDeep을 세우고 판정을 멈춘다(그
// 원문은 어느 쪽으로 읽어도 오류이며, 타입 인자로 읽으면 분석이 그 자리에 S0215를 낸다).
bool SyntaxTreeBuilder::ScanTypeArguments(usize& index, uint32 depth,
                                          bool& tooDeep) const noexcept {
    if (depth >= MaxSyntaxNesting) {
        tooDeep = true;
        return true;
    }

    index = SkipDocs(index + 1);
    while (true) {
        if (!ScanType(index, depth + 1, tooDeep)) {
            return false;
        }

        if (tooDeep) {
            return true;
        }

        const TokenKind kind = TokenAt(index).kind;
        if (kind == TokenKind::Comma) {
            index = SkipDocs(index + 1);
            continue;
        }

        if (kind == TokenKind::Greater) {
            index = SkipDocs(index + 1);
            return true;
        }

        return false;
    }
}

bool SyntaxTreeBuilder::ScanType(usize& index, uint32 depth, bool& tooDeep) const noexcept {
    if (TokenAt(index).kind != TokenKind::Identifier) {
        return false;
    }

    index = SkipDocs(index + 1);
    while (TokenAt(index).kind == TokenKind::Dot) {
        const usize next = SkipDocs(index + 1);
        if (TokenAt(next).kind != TokenKind::Identifier) {
            return false;
        }

        index = SkipDocs(next + 1);
    }

    if (TokenAt(index).kind == TokenKind::Less) {
        if (!ScanTypeArguments(index, depth, tooDeep)) {
            return false;
        }

        if (tooDeep) {
            return true;
        }
    }

    if (TokenAt(index).kind == TokenKind::Question) {
        index = SkipDocs(index + 1);
    }

    return true;
}

} // namespace rocks::sparks::detail
