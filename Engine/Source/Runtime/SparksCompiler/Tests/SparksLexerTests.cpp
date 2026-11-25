#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Lexer.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/Test.h>

// Sparks 어휘 분석 시험이다(Docs/Design/SparksLanguage 8.1절 "어휘 분석" 표·"토큰", 3.6절
// 어휘): 토큰 종류와 범위, 문자열·보간 분할, 숫자 규칙(SD54), verbatim 문자열(SD55), 주석,
// S0104–S0114 진단과 같은 줄 중복 억제, 오류 뒤 복구.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_lexer_tests_file {

namespace {

using K = TokenKind;

// text를 어휘 분석한 결과다.
struct Lexed {
    explicit Lexed(Allocator& allocator) noexcept
        : sources(allocator), diagnostics(allocator), tokens(allocator) {
    }

    SourceSet sources;
    DiagnosticList diagnostics;
    Array<Token> tokens;
    SourceId id;
};

bool Lex(Lexed& lexed, StringView text) noexcept {
    auto id = lexed.sources.TryAdd("Lex.spark", text, lexed.diagnostics);
    if (!id) {
        return false;
    }

    lexed.id = id.Value();
    return static_cast<bool>(Tokenize(lexed.sources, lexed.id, lexed.diagnostics, lexed.tokens));
}

bool KindsAre(const Lexed& lexed, Span<const TokenKind> expected) noexcept {
    if (lexed.tokens.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (lexed.tokens[index].kind != expected[index]) {
            return false;
        }
    }

    return true;
}

StringView TokenText(const Lexed& lexed, usize index) noexcept {
    const SourceSpan span = lexed.tokens[index].span;
    return StringView(lexed.sources.Get(lexed.id).Text().Data() + span.begin,
                      span.end - span.begin);
}

// 진단 하나가 code이고 그 위치가 line:column인지 본다.
bool DiagnosticIs(const Lexed& lexed, usize index, DiagnosticCode code, uint32 line,
                  uint32 column) noexcept {
    if (index >= lexed.diagnostics.Count()) {
        return false;
    }

    const Diagnostic& diagnostic = lexed.diagnostics[index];
    const sparks::SourceLocation location =
        lexed.sources.Get(lexed.id).Locate(diagnostic.Span().begin);
    return diagnostic.Code() == code && location.line == line && location.column == column;
}

} // namespace

TEST(SparksLexer_EmptySourceHasOnlyEndOfFile) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, ""));
        const TokenKind expected[] = {K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.diagnostics.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_NamesKeywordsAndAsQuestion) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "import Weapons.Sword as Blades;\nx as? Foo _ asx true null open"));
        const TokenKind expected[] = {K::Import,     K::Identifier, K::Dot,        K::Identifier,
                                      K::As,         K::Identifier, K::Semicolon,  K::Identifier,
                                      K::AsQuestion, K::Identifier, K::Underscore, K::Identifier,
                                      K::True,       K::Null,       K::Open,       K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.diagnostics.IsEmpty());
        CHECK(TokenText(lexed, 8) == StringView("as?"));
        CHECK(TokenText(lexed, 11) == StringView("asx"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_AllPunctuation) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "{ } ( ) [ ] ; , . ?. ? : = == != < <= > >= + - * / % += -= *= /= ! "
                           "&& || ?? ..< @"));
        const TokenKind expected[] = {K::LeftBrace,    K::RightBrace,
                                      K::LeftParen,    K::RightParen,
                                      K::LeftBracket,  K::RightBracket,
                                      K::Semicolon,    K::Comma,
                                      K::Dot,          K::QuestionDot,
                                      K::Question,     K::Colon,
                                      K::Equal,        K::EqualEqual,
                                      K::BangEqual,    K::Less,
                                      K::LessEqual,    K::Greater,
                                      K::GreaterEqual, K::Plus,
                                      K::Minus,        K::Star,
                                      K::Slash,        K::Percent,
                                      K::PlusEqual,    K::MinusEqual,
                                      K::StarEqual,    K::SlashEqual,
                                      K::Bang,         K::AmpersandAmpersand,
                                      K::PipePipe,     K::QuestionQuestion,
                                      K::DotDotLess,   K::At,
                                      K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.diagnostics.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 숫자는 최신 C# 형태다(SD54): 구분자 `_`, 16진·2진 접두사, `.5`, 앞자리 0(10진). 숫자 뒤의 `.`는
// 다음 문자가 숫자일 때만 실수에 들어간다(`0..<n`은 범위).
TEST(SparksLexer_NumbersAndRanges) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "0 12 1_000 1__0 007 0xFF_FF 0x_1 0B1010_0101 1.5 .5 1e3 1.5E-3 2e+1 "
                           "1_000.5e-1_0 0..<n x.Count 9223372036854775808"));
        const TokenKind expected[] = {K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::IntegerLiteral,
                                      K::FloatLiteral,
                                      K::FloatLiteral,
                                      K::FloatLiteral,
                                      K::FloatLiteral,
                                      K::FloatLiteral,
                                      K::FloatLiteral,
                                      K::IntegerLiteral,
                                      K::DotDotLess,
                                      K::Identifier,
                                      K::Identifier,
                                      K::Dot,
                                      K::Identifier,
                                      K::IntegerLiteral,
                                      K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(TokenText(lexed, 7) == StringView("0B1010_0101"));
        CHECK(TokenText(lexed, 13) == StringView("1_000.5e-1_0"));
        // 정수 범위(S0109)는 쓰는 자리의 타입을 보는 검사가 판정한다.
        CHECK(lexed.diagnostics.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 형식이 틀린 숫자는 S0110, C# 접미사는 S0114, 유한하지 않은 실수는 S0113이다. 각 줄은 숫자 토큰
// 하나로 복구된다.
TEST(SparksLexer_MalformedNumbers) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "1.\n1e\n1e+\n12abc\n1e400\n1_\n1_.5\n1._5\n1e_5\n0x\n0b102\n0xFF_\n"
                           "1L\n1.5f\n0xFFu\n2d\n3m\n4UL"));
        struct Expected {
            DiagnosticCode code;
            TokenKind kind;
        };
        const Expected expected[] = {
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1.
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1e
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1e+
            {DiagnosticCode::MalformedNumber, K::IntegerLiteral},      // 12abc
            {DiagnosticCode::FloatLiteralOutOfRange, K::FloatLiteral}, // 1e400
            {DiagnosticCode::MalformedNumber, K::IntegerLiteral},      // 1_
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1_.5
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1._5
            {DiagnosticCode::MalformedNumber, K::FloatLiteral},        // 1e_5
            {DiagnosticCode::MalformedNumber, K::IntegerLiteral},      // 0x
            {DiagnosticCode::MalformedNumber, K::IntegerLiteral},      // 0b102
            {DiagnosticCode::MalformedNumber, K::IntegerLiteral},      // 0xFF_
            {DiagnosticCode::NumericSuffix, K::IntegerLiteral},        // 1L
            {DiagnosticCode::NumericSuffix, K::FloatLiteral},          // 1.5f
            {DiagnosticCode::NumericSuffix, K::IntegerLiteral},        // 0xFFu
            {DiagnosticCode::NumericSuffix, K::IntegerLiteral},        // 2d
            {DiagnosticCode::NumericSuffix, K::IntegerLiteral},        // 3m
            {DiagnosticCode::NumericSuffix, K::IntegerLiteral},        // 4UL
        };
        constexpr usize Count = sizeof(expected) / sizeof(expected[0]);
        REQUIRE(lexed.diagnostics.Count() == Count);
        REQUIRE(lexed.tokens.Size() == Count + 1);
        for (usize index = 0; index < Count; ++index) {
            CHECK(DiagnosticIs(lexed, index, expected[index].code, static_cast<uint32>(index + 1),
                               1));
            CHECK(lexed.tokens[index].kind == expected[index].kind);
        }

        CHECK(lexed.diagnostics[0].Fix() == StringView("Write '1.0' instead of '1.'."));
        CHECK(lexed.diagnostics[5].Explanation() ==
              StringView("Digit separators '_' go between digits (or right after '0x' or '0b')."));
        CHECK(lexed.diagnostics[10].Explanation() ==
              StringView("Binary literals use only the digits 0 and 1."));
        CHECK(lexed.diagnostics[12].Message() ==
              StringView("Numeric suffixes are not used in Sparks."));
        CHECK(TokenText(lexed, 3) == StringView("12abc"));
        CHECK(TokenText(lexed, 14) == StringView("0xFFu"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// verbatim 문자열(SD55)은 `@"`부터 닫는 `"`까지 한 토큰이다: `\`는 글자, `""`는 따옴표, `${`는
// 보간이 아니며 여러 줄일 수 있다. `@` 뒤에 공백이 있으면 attribute 기호와 일반 문자열이다.
TEST(SparksLexer_VerbatimStrings) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(
            Lex(lexed, "@\"C:\\Games\\${x}\" @\"say \"\"hi\"\"\" @\"line1\nline2\" @ \"a\" @edit"));
        const TokenKind expected[] = {
            K::VerbatimString, K::VerbatimString, K::VerbatimString, K::At,
            K::StringBegin,    K::StringText,     K::StringEnd,      K::At,
            K::Identifier,     K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.diagnostics.IsEmpty());
        CHECK(TokenText(lexed, 0) == StringView("@\"C:\\Games\\${x}\""));
        CHECK(TokenText(lexed, 1) == StringView("@\"say \"\"hi\"\"\""));
        CHECK(TokenText(lexed, 2) == StringView("@\"line1\nline2\""));

        // 닫히지 않은 verbatim 문자열은 파일 끝까지 가며 위치는 `@`다.
        Lexed open(allocator);
        REQUIRE(Lex(open, "x = @\"never\nclosed"));
        REQUIRE(open.diagnostics.Count() == 1);
        CHECK(DiagnosticIs(open, 0, DiagnosticCode::UnterminatedString, 1, 5));
        const TokenKind openKinds[] = {K::Identifier, K::Equal, K::VerbatimString, K::Invalid,
                                       K::EndOfFile};
        CHECK(KindsAre(open, openKinds));

        // 보간 안의 verbatim 문자열도 S0111이다.
        Lexed nested(allocator);
        REQUIRE(Lex(nested, "\"${@\"x\"}\""));
        REQUIRE(nested.diagnostics.Count() == 1);
        CHECK(DiagnosticIs(nested, 0, DiagnosticCode::StringInInterpolation, 1, 4));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_StringsAndInterpolation) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "\"HP ${Current}/${Max}\" \"\" \"a$b\\${c}\" \"${ { } }\""));
        const TokenKind expected[] = {
            K::StringBegin,        K::StringText, K::InterpolationBegin, K::Identifier,
            K::InterpolationEnd,   K::StringText, K::InterpolationBegin, K::Identifier,
            K::InterpolationEnd,   K::StringEnd,  K::StringBegin,        K::StringEnd,
            K::StringBegin,        K::StringText, K::StringEnd,          K::StringBegin,
            K::InterpolationBegin, K::LeftBrace,  K::RightBrace,         K::InterpolationEnd,
            K::StringEnd,          K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.diagnostics.IsEmpty());
        CHECK(TokenText(lexed, 1) == StringView("HP "));
        CHECK(TokenText(lexed, 5) == StringView("/"));
        // 단독 `$`와 `\${`는 문자열 조각 안에 원문 그대로 남는다(escape는 구문 분석이 푼다).
        CHECK(TokenText(lexed, 13) == StringView("a$b\\${c}"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_EscapesAreValidated) {
    SystemAllocator allocator;
    {
        Lexed valid(allocator);
        REQUIRE(Lex(valid, "\"\\\" \\\\ \\n \\t \\$ \\u{41} \\u{1F600} \\u{10FFFF}\""));
        CHECK(valid.diagnostics.IsEmpty());

        Lexed invalid(allocator);
        REQUIRE(Lex(invalid, "\"\\q\"\n\"\\u{}\"\n\"\\u{1234567}\"\n\"\\u{D800}\"\n"
                             "\"\\u{110000}\"\n\"\\u{41\""));
        REQUIRE(invalid.diagnostics.Count() == 6);
        for (uint32 line = 1; line <= 6; ++line) {
            CHECK(DiagnosticIs(invalid, line - 1, DiagnosticCode::InvalidEscape, line, 2));
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 닫히지 않은 문자열은 여는 `"` 위치의 S0107이고 줄 끝에서 닫힌 것으로 본다. 보간 안에서 줄이
// 바뀌어도 같다. CRLF의 CR은 문자열 조각에 들어가지 않는다.
TEST(SparksLexer_UnterminatedStrings) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "x = \"abc\r\ny = \"${a\nz"));
        REQUIRE(lexed.diagnostics.Count() == 2);
        CHECK(DiagnosticIs(lexed, 0, DiagnosticCode::UnterminatedString, 1, 5));
        CHECK(DiagnosticIs(lexed, 1, DiagnosticCode::UnterminatedString, 2, 5));
        const TokenKind expected[] = {
            K::Identifier, K::Equal,      K::StringBegin, K::StringText,  K::Invalid,
            K::StringEnd,  K::Identifier, K::Equal,       K::StringBegin, K::InterpolationBegin,
            K::Identifier, K::Invalid,    K::StringEnd,   K::Identifier,  K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(TokenText(lexed, 3) == StringView("abc"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_StringInsideInterpolationIsAnError) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "\"${f(\"x\")}\""));
        REQUIRE(lexed.diagnostics.Count() == 1);
        CHECK(DiagnosticIs(lexed, 0, DiagnosticCode::StringInInterpolation, 1, 6));
        // 안쪽 문자열도 토큰으로 읽어 다시 맞춘다.
        const TokenKind expected[] = {K::StringBegin, K::InterpolationBegin, K::Identifier,
                                      K::LeftParen,   K::StringBegin,        K::StringText,
                                      K::StringEnd,   K::RightParen,         K::InterpolationEnd,
                                      K::StringEnd,   K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_Comments) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "a // line\n/// doc\r\n//// not doc\n///\nb /* block\n */ c"));
        const TokenKind expected[] = {K::Identifier, K::DocComment, K::DocComment,
                                      K::Identifier, K::Identifier, K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(TokenText(lexed, 1) == StringView("/// doc"));
        CHECK(TokenText(lexed, 2) == StringView("///"));
        CHECK(lexed.diagnostics.IsEmpty());

        Lexed open(allocator);
        REQUIRE(Lex(open, "a /* not closed\nb"));
        REQUIRE(open.diagnostics.Count() == 1);
        CHECK(DiagnosticIs(open, 0, DiagnosticCode::UnterminatedBlockComment, 1, 3));
        const TokenKind onlyA[] = {K::Identifier, K::Invalid, K::EndOfFile};
        CHECK(KindsAre(open, onlyA));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 쓸 수 없는 문자는 S0104이고 Invalid 토큰으로 남긴다. 같은 줄의 같은 코드는 한 번만 낸다.
TEST(SparksLexer_InvalidCharactersAreReportedOncePerLine) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "a # ' b\nc & d\ne | f\n\xED\x95\x9C x\n$ ` ~ ^ \\"));
        REQUIRE(lexed.diagnostics.Count() == 5);
        CHECK(DiagnosticIs(lexed, 0, DiagnosticCode::InvalidCharacter, 1, 3));
        CHECK(lexed.diagnostics[0].Message() == StringView("Unexpected character '#'."));
        CHECK(DiagnosticIs(lexed, 1, DiagnosticCode::InvalidCharacter, 2, 3));
        CHECK(lexed.diagnostics[1].Fix() == StringView("Use '&&' for logical and."));
        CHECK(DiagnosticIs(lexed, 2, DiagnosticCode::InvalidCharacter, 3, 3));
        CHECK(lexed.diagnostics[2].Fix() == StringView("Use '||' for logical or."));
        CHECK(DiagnosticIs(lexed, 3, DiagnosticCode::InvalidCharacter, 4, 1));
        CHECK(lexed.diagnostics[3].Message() == StringView("Unexpected character U+D55C."));
        CHECK(DiagnosticIs(lexed, 4, DiagnosticCode::InvalidCharacter, 5, 1));
        const TokenKind expected[] = {
            K::Identifier, K::Invalid,    K::Invalid, K::Identifier, K::Identifier, K::Invalid,
            K::Identifier, K::Identifier, K::Invalid, K::Identifier, K::Invalid,    K::Identifier,
            K::Invalid,    K::Invalid,    K::Invalid, K::Invalid,    K::Invalid,    K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_UnderscoreInIdentifiersSuggestsAName) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "move_speed\n_x\n__\nMax_Hp_"));
        REQUIRE(lexed.diagnostics.Count() == 4);
        CHECK(DiagnosticIs(lexed, 0, DiagnosticCode::UnderscoreInIdentifier, 1, 1));
        CHECK(lexed.diagnostics[0].Fix() == StringView("Rename to 'moveSpeed'."));
        CHECK(lexed.diagnostics[1].Fix() == StringView("Rename to 'x'."));
        CHECK(lexed.diagnostics[2].Fix() == StringView("Use a name made of letters and digits."));
        CHECK(lexed.diagnostics[3].Fix() == StringView("Rename to 'MaxHp'."));
        const TokenKind expected[] = {K::Identifier, K::Identifier, K::Identifier, K::Identifier,
                                      K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_DoubleDotSuggestsTheRangeOperator) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "for (i in 0..n)"));
        REQUIRE(lexed.diagnostics.Count() == 1);
        CHECK(DiagnosticIs(lexed, 0, DiagnosticCode::RangeOperator, 1, 12));
        const TokenKind expected[] = {K::For,        K::LeftParen,      K::Identifier,
                                      K::In,         K::IntegerLiteral, K::DotDotLess,
                                      K::Identifier, K::RightParen,     K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// BOM 뒤부터 읽는다. 토큰 범위는 원문(BOM 포함) 기준 위치다.
TEST(SparksLexer_StartsAfterTheBom) {
    SystemAllocator allocator;
    {
        Lexed lexed(allocator);
        REQUIRE(Lex(lexed, "\xEF\xBB\xBFlet"));
        const TokenKind expected[] = {K::Let, K::EndOfFile};
        CHECK(KindsAre(lexed, expected));
        CHECK(lexed.tokens[0].span.begin == Utf8BomBytes);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_RejectsUnusableSources) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        Array<Token> tokens(allocator);
        auto id = sources.TryAdd("Bad.spark", "\xFF", diagnostics);
        REQUIRE(id);
        auto lexed = Tokenize(sources, id.Value(), diagnostics, tokens);
        CHECK(!lexed && lexed.Error() == ErrorCode::InvalidArgument);
        CHECK(tokens.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLexer_TokenKindTextQuotesPunctuationAndKeywords) {
    CHECK(TokenKindText(TokenKind::LeftBrace) == StringView("'{'"));
    CHECK(TokenKindText(TokenKind::Class) == StringView("'class'"));
    CHECK(TokenKindText(TokenKind::Identifier) == StringView("identifier"));
    CHECK(TokenKindText(TokenKind::VerbatimString) == StringView("verbatim string literal"));
    CHECK(IsKeyword(TokenKind::Import) && IsKeyword(TokenKind::Open));
    CHECK(!IsKeyword(TokenKind::Identifier) && !IsKeyword(TokenKind::LeftBrace));
}

} // namespace sparks_lexer_tests_file
