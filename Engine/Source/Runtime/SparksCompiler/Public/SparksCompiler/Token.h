#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>

// Sparks 토큰이다(Docs/Design/SparksLanguage.ko.md 8.1절 "어휘 분석"·"토큰", 3.6절 어휘).
//
// 토큰은 종류와 원문 위치만 가진다. 값(정수·실수, escape를 푼 문자열)은 구문 분석이 원문에서 다시
// 읽는다. 문자열은 여러 토큰으로 나뉜다:
//   "HP ${Current}/${Max}"  →  StringBegin, StringText("HP "), InterpolationBegin, Identifier,
//                              InterpolationEnd, StringText("/"), InterpolationBegin, Identifier,
//                              InterpolationEnd, StringEnd
// StringText의 범위는 escape를 풀지 않은 원문이다. 비어 있는 조각은 만들지 않는다.

namespace rocks::sparks {

enum class TokenKind : uint8 {
    EndOfFile,
    Identifier,
    Underscore, // 단독 `_`(버림·생략)
    IntegerLiteral,
    FloatLiteral,
    StringBegin,        // 여는 `"`
    StringText,         // 문자열 조각(escape 포함 원문)
    InterpolationBegin, // `${`
    InterpolationEnd,   // 보간을 닫는 `}`
    StringEnd,          // 닫는 `"`(닫히지 않은 문자열이면 빈 범위)
    DocComment,         // `///` 줄 전체
    VerbatimString,     // `@"..."` 전체(escape·보간 없음, 여러 줄 가능)

    // 키워드(부록 A).
    Import,
    As,
    Class,
    Struct,
    Enum,
    Event,
    Fn,
    Async,
    Const,
    Var,
    Let,
    If,
    Else,
    For,
    In,
    While,
    Switch,
    Case,
    Default,
    Break,
    Continue,
    Return,
    On,
    Emit,
    Await,
    Try,
    Override,
    This,
    True,
    False,
    Null,
    Interface,
    Open,

    // 구두점.
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Semicolon,
    Comma,
    Dot,
    QuestionDot,
    Question,
    Colon,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    Bang,
    AmpersandAmpersand,
    PipePipe,
    QuestionQuestion,
    DotDotLess,
    At,
    AsQuestion // `as?`
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    SourceSpan span;
};

// 진단 문장에 쓰는 토큰 표기다(예: "'{'", "identifier", "'class'"). 영어다.
StringView TokenKindText(TokenKind kind) noexcept;

// 키워드 토큰인지다.
bool IsKeyword(TokenKind kind) noexcept;

} // namespace rocks::sparks
