#include "SyntaxTreeBuilder.h"

// 블록과 문장의 구문 분석이다(3.6절 Block·Stmt·Value·LocalDecl 이하, 8.1절 "문법 세부").

namespace rocks::sparks::detail {

// Block = "{" { Stmt } "}". `{`가 없으면 Error 노드다.
uint32 SyntaxTreeBuilder::ParseBlock(StringView context, StringView fix) noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    if (!Expect(TokenKind::LeftBrace, context, fix)) {
        return MakeError();
    }

    panic_ = false;
    while (PeekKind(0) != TokenKind::RightBrace && PeekKind(0) != TokenKind::EndOfFile &&
           error_ == ErrorCode::None) {
        const usize before = position_;
        const usize consumed = consumed_;
        Push(ParseStatement());
        Recover(SyncLevel::Statement, consumed);
        if (position_ == before) {
            SkipOne();
        }
    }

    Expect(TokenKind::RightBrace, "to close the block");
    return Make(SyntaxKind::Block, mark, begin);
}

uint32 SyntaxTreeBuilder::ParseStatement() noexcept {
    Nesting nesting(*this);
    if (nesting.TooDeep()) {
        return MakeError();
    }

    if (At(TokenKind::At)) {
        DiscardAttributes("Statements cannot have attributes.");
        // 뒤에 문장이 없으면 S0214 하나로 둔다(멤버·선언 자리의 `@edit }`와 같게 진단 하나).
        const TokenKind next = Kind();
        if (next != TokenKind::Underscore && !IsStatementKeyword(next) && !StartsValue(next)) {
            panic_ = true;
        }
    }

    switch (Kind()) {
    case TokenKind::Var:
    case TokenKind::Let:
        return ParseLocalVariable();
    case TokenKind::If:
        return ParseIf();
    case TokenKind::For:
        return ParseFor();
    case TokenKind::While:
        return ParseWhile();
    case TokenKind::Switch:
        return ParseSwitch();
    case TokenKind::Return:
        return ParseReturn();
    case TokenKind::Emit:
        return ParseEmit();
    case TokenKind::Break:
    case TokenKind::Continue:
        return ParseJump();
    case TokenKind::Semicolon:
        ReportInPlace(DiagnosticCode::ExpectedStatement, CurrentSpan(), "Empty statement.",
                      "A ';' on its own does nothing.", "Remove the extra ';'.");
        return SkipErrorToken(false);
    case TokenKind::LeftBrace:
        ReportAtCurrent(DiagnosticCode::ExpectedStatement, "A block cannot stand alone.",
                        "Braces only follow if, for, while, switch, case, default, functions and "
                        "handlers.",
                        "Remove the braces or add the statement that owns the block.");
        return SkipErrorToken(false);
    case TokenKind::Underscore:
        if (PeekKind(1) == TokenKind::Equal) {
            return ParseDiscardAssignment();
        }

        break;
    default:
        break;
    }

    const TokenKind kind = Kind();
    if (StartsValue(kind)) {
        return ParseExpressionStatement();
    }

    if (IsDeclarationStart(kind) || kind == TokenKind::On) {
        ReportExpected(DiagnosticCode::ExpectedStatement, "a statement", StringView(),
                       "Declarations cannot appear inside a function body.",
                       "Move the declaration out of the function.");
    } else if (kind == TokenKind::Else) {
        ReportExpected(DiagnosticCode::ExpectedStatement, "a statement", StringView(),
                       "'else' must follow the closing '}' of an 'if' body.",
                       "Attach 'else' to an 'if'.");
    } else {
        ReportExpected(DiagnosticCode::ExpectedStatement, "a statement", StringView(), StringView(),
                       "Write a statement or close the block with '}'.");
    }

    return SkipErrorToken(false);
}

// LocalDecl = ( "var" | "let" ) Ident [ ":" Type ] "=" Value [ "??" Exit ] ";"
uint32 SyntaxTreeBuilder::ParseLocalVariable() noexcept {
    const uint32 begin = CurrentBegin();
    const uint32 flags = At(TokenKind::Let) ? SyntaxFlags::Let : 0;
    Advance();
    const usize mark = Mark();
    Push(ParseName("for the local variable"));
    Push(Accept(TokenKind::Colon) ? ParseType("for the local variable") : NoNode);
    Push(Expect(TokenKind::Equal, "in the local variable",
                "Give a local variable its value where it is declared, as in 'var count = 0;'.")
             ? ParseValue()
             : MakeError());
    if (At(TokenKind::QuestionQuestion) && PeekKind(1) == TokenKind::Return) {
        const uint32 exitBegin = CurrentBegin();
        Advance();
        Advance();
        const usize exitMark = Mark();
        Push(At(TokenKind::Semicolon) ? NoNode : ParseExpression());
        Push(Make(SyntaxKind::ExitReturn, exitMark, exitBegin));
    } else {
        Push(NoNode);
    }

    Expect(TokenKind::Semicolon, "after the local variable");
    return Make(SyntaxKind::LocalVariable, mark, begin, NoToken, flags);
}

// Value = Expr | "try" [ "await" ] Postfix | "await" Postfix
uint32 SyntaxTreeBuilder::ParseValue() noexcept {
    if (!At(TokenKind::Try) && !At(TokenKind::Await)) {
        return ParseExpression();
    }

    const SourceSpan keyword = CurrentSpan();
    const uint32 value = ParseTryAwait();
    if (AtBinaryOperator() || At(TokenKind::AsQuestion)) {
        // §3.3: try·await는 바로 뒤의 호출 하나에 붙는다(`await F() + 1`은 오류).
        Report(DiagnosticCode::MisplacedAwait, keyword,
               "'await' and 'try' cannot be combined with an operator.",
               "They apply to the one call right after them, so the rest of the expression "
               "needs the value first.",
               "Store the value in a local variable first, as in 'let v = await F(); let x = v "
               "+ 1;'.");
    }

    return value;
}

bool SyntaxTreeBuilder::AtBinaryOperator() noexcept {
    switch (Kind()) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::AmpersandAmpersand:
    case TokenKind::PipePipe:
        return true;
    case TokenKind::QuestionQuestion:
        return !IsExitKeyword(PeekKind(1));
    default:
        return false;
    }
}

uint32 SyntaxTreeBuilder::ParseTryAwait() noexcept {
    Nesting nesting(*this);
    if (nesting.TooDeep()) {
        return MakeError();
    }

    const uint32 begin = CurrentBegin();
    if (Accept(TokenKind::Try)) {
        const uint32 flags = Accept(TokenKind::Await) ? SyntaxFlags::Await : 0;
        const usize mark = Mark();
        Push(ParsePostfix());
        return Make(SyntaxKind::Try, mark, begin, NoToken, flags);
    }

    Advance();
    const usize mark = Mark();
    Push(ParsePostfix());
    return Make(SyntaxKind::Await, mark, begin);
}

// Postfix "=" Value ";" | Postfix CompoundOp Expr ";" | Value ";"
uint32 SyntaxTreeBuilder::ParseExpressionStatement() noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    const uint32 value = ParseValue();
    const TokenKind kind = Kind();
    if (kind != TokenKind::Equal && !IsCompoundAssignment(kind)) {
        Push(value);
        Expect(TokenKind::Semicolon, "after the statement");
        return Make(SyntaxKind::ExpressionStatement, mark, begin);
    }

    const uint32 op = Advance();
    if (!IsAssignable(NodeKind(value)) && NodeKind(value) != SyntaxKind::Error) {
        ReportInPlace(DiagnosticCode::InvalidAssignmentTarget, NodeSpan(value),
                      "This cannot be assigned to.",
                      "The left side of an assignment is a variable, a field or an element.",
                      "Assign to a variable or a field, as in 'count = count + 1;'.");
    }

    Push(value);
    if (kind != TokenKind::Equal && (At(TokenKind::Try) || At(TokenKind::Await))) {
        ReportInPlace(DiagnosticCode::MisplacedAwait, CurrentSpan(),
                      "'await' and 'try' cannot follow a compound assignment.",
                      "They are allowed on the right of '=' but not of '+=', '-=', '*=' or '/='.",
                      "Store the value in a local variable first.");
    }

    Push(ParseValue());
    Expect(TokenKind::Semicolon, "after the assignment");
    return Make(SyntaxKind::Assignment, mark, begin, op);
}

// "_" "=" Value ";"
uint32 SyntaxTreeBuilder::ParseDiscardAssignment() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    Advance();
    const usize mark = Mark();
    Push(ParseValue());
    Expect(TokenKind::Semicolon, "after the assignment");
    return Make(SyntaxKind::DiscardAssignment, mark, begin);
}

// If = "if" "(" ( Expr | "let" Ident "=" Expr ) ")" Block [ "else" ( If | Block ) ]. `else if`도
// 중첩 한 단계로 센다(트리 깊이 상한을 뒤 단계가 믿을 수 있게).
uint32 SyntaxTreeBuilder::ParseIf() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Expect(TokenKind::LeftParen, "after 'if'");
    if (At(TokenKind::Let)) {
        const uint32 letBegin = CurrentBegin();
        Advance();
        const usize letMark = Mark();
        Push(ParseName("after 'let'"));
        Push(Expect(TokenKind::Equal, "after the name") ? ParseExpression() : MakeError());
        Push(Make(SyntaxKind::IfLet, letMark, letBegin));
    } else {
        Push(ParseExpression());
    }

    Expect(TokenKind::RightParen, "to close the condition");
    Push(ParseBlock("to start the body of 'if'",
                    "Wrap the body in braces, as in 'if (ready) { Open(); }'."));
    if (!Accept(TokenKind::Else)) {
        Push(NoNode);
    } else if (At(TokenKind::If)) {
        Nesting nesting(*this);
        Push(nesting.TooDeep() ? MakeError() : ParseIf());
    } else {
        Push(ParseBlock("after 'else'", "Wrap the body in braces, as in 'else { Close(); }'."));
    }

    return Make(SyntaxKind::If, mark, begin);
}

// Binder = Ident | "_"
uint32 SyntaxTreeBuilder::ParseBinder(StringView context) noexcept {
    if (At(TokenKind::Underscore)) {
        return Leaf(SyntaxKind::Discard, Advance());
    }

    return ParseName(context);
}

// For = "for" "(" Binder [ "," Binder ] "in" Expr [ "..<" Expr ] ")" Block
uint32 SyntaxTreeBuilder::ParseFor() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Expect(TokenKind::LeftParen, "after 'for'");
    Push(ParseBinder("for the loop variable"));
    Push(Accept(TokenKind::Comma) ? ParseBinder("for the second loop variable") : NoNode);
    Expect(TokenKind::In, "after the loop variable",
           "Write the loop as 'for (item in items) { ... }'.");
    Push(ParseExpression());
    Push(Accept(TokenKind::DotDotLess) ? ParseExpression() : NoNode);
    Expect(TokenKind::RightParen, "to close the loop header");
    Push(ParseBlock("to start the body of 'for'",
                    "Wrap the body in braces, as in 'for (i in 0..<count) { ... }'."));
    return Make(SyntaxKind::For, mark, begin);
}

// While = "while" "(" Expr ")" Block
uint32 SyntaxTreeBuilder::ParseWhile() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Expect(TokenKind::LeftParen, "after 'while'");
    Push(ParseExpression());
    Expect(TokenKind::RightParen, "to close the condition");
    Push(ParseBlock("to start the body of 'while'",
                    "Wrap the body in braces, as in 'while (running) { Step(); }'."));
    return Make(SyntaxKind::While, mark, begin);
}

// Switch = "switch" "(" Expr ")" "{" { "case" Expr { "," Expr } ":" Block } [ "default" ":" Block ]
// "}". `default`가 마지막이 아니거나 두 번 나오면 S0217이다(두 번째 default는 트리에 넣지 않는다).
uint32 SyntaxTreeBuilder::ParseSwitch() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Expect(TokenKind::LeftParen, "after 'switch'");
    Push(ParseExpression());
    Expect(TokenKind::RightParen, "to close the switch value");
    const uint32 casesBegin = CurrentBegin();
    const usize casesMark = Mark();
    uint32 defaultNode = NoNode;
    if (Expect(TokenKind::LeftBrace, "to start the switch body")) {
        panic_ = false;
        bool reportedLateCase = false;
        while (PeekKind(0) != TokenKind::RightBrace && PeekKind(0) != TokenKind::EndOfFile &&
               error_ == ErrorCode::None) {
            const usize before = position_;
            const usize consumed = consumed_;
            if (At(TokenKind::Case)) {
                if (defaultNode != NoNode && !reportedLateCase) {
                    reportedLateCase = true;
                    ReportInPlace(DiagnosticCode::MisplacedDefault, CurrentSpan(),
                                  "'default' must be the last part of a switch.",
                                  "'default' handles every value the cases do not list, so it "
                                  "comes after all of them.",
                                  "Move 'default' to the end.");
                }

                Push(ParseSwitchCase());
            } else if (At(TokenKind::Default)) {
                if (defaultNode == NoNode) {
                    defaultNode = ParseSwitchDefault();
                } else {
                    ReportInPlace(DiagnosticCode::MisplacedDefault, CurrentSpan(),
                                  "A switch has only one 'default'.",
                                  "The second 'default' would never run.",
                                  "Remove the extra 'default'.");
                    const NodeCheckpoint checkpoint = SaveNodes();
                    static_cast<void>(ParseSwitchDefault());
                    RestoreNodes(checkpoint);
                }
            } else {
                ReportExpected(DiagnosticCode::ExpectedToken, "'case' or 'default'",
                               "in the switch body", StringView(),
                               "Start each part with 'case Value:' or 'default:'.");
            }

            RecoverSwitchPart(consumed);
            if (position_ == before) {
                SkipOne();
            }
        }

        Expect(TokenKind::RightBrace, "to close the switch body");
    }

    Push(Make(SyntaxKind::List, casesMark, casesBegin));
    Push(defaultNode);
    return Make(SyntaxKind::Switch, mark, begin);
}

uint32 SyntaxTreeBuilder::ParseSwitchCase() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    const uint32 valuesBegin = CurrentBegin();
    const usize valuesMark = Mark();
    do {
        Push(ParseExpression());
    } while (Accept(TokenKind::Comma));

    Push(Make(SyntaxKind::List, valuesMark, valuesBegin));
    Expect(TokenKind::Colon, "after the case values");
    Push(ParseBlock("for the case body",
                    "Wrap the case body in braces, as in 'case 1: { Open(); }'."));
    return Make(SyntaxKind::SwitchCase, mark, begin);
}

uint32 SyntaxTreeBuilder::ParseSwitchDefault() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Expect(TokenKind::Colon, "after 'default'");
    Push(ParseBlock("for the default body",
                    "Wrap the default body in braces, as in 'default: { Close(); }'."));
    return Make(SyntaxKind::SwitchDefault, mark, begin);
}

// switch 본문의 동기화 지점은 `case`·`default`·`}`다. 그 부분이 `}`로 끝났으면 건너뛸 것이 없다.
void SyntaxTreeBuilder::RecoverSwitchPart(usize consumedBefore) noexcept {
    if (!panic_ || stopped_ || PeekKind(0) == TokenKind::EndOfFile) {
        return;
    }

    if (consumed_ > consumedBefore && lastKind_ == TokenKind::RightBrace) {
        panic_ = false;
        return;
    }

    while (true) {
        const TokenKind kind = PeekKind(0);
        if (kind == TokenKind::EndOfFile) {
            return;
        }

        if (kind == TokenKind::Case || kind == TokenKind::Default ||
            kind == TokenKind::RightBrace) {
            break;
        }

        if (kind == TokenKind::LeftBrace) {
            SkipBalancedBraces();
            continue;
        }

        SkipOne();
    }

    panic_ = false;
}

// Return = "return" [ Value ] ";"
uint32 SyntaxTreeBuilder::ParseReturn() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Push(At(TokenKind::Semicolon) ? NoNode : ParseValue());
    Expect(TokenKind::Semicolon, "after the return");
    return Make(SyntaxKind::Return, mark, begin);
}

// Emit = "emit" EvPath "(" [ Expr { "," Expr } ] ")" ";"
uint32 SyntaxTreeBuilder::ParseEmit() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Push(ParseEventPath("after 'emit'"));
    const uint32 argumentsBegin = CurrentBegin();
    const usize argumentsMark = Mark();
    if (Expect(TokenKind::LeftParen, "after the event name",
               "Pass the event arguments in parentheses, as in 'emit Opened(by);'.")) {
        if (!At(TokenKind::RightParen)) {
            do {
                Push(ParseExpression());
            } while (Accept(TokenKind::Comma));
        }

        Expect(TokenKind::RightParen, "to close the event arguments");
    }

    Push(Make(SyntaxKind::List, argumentsMark, argumentsBegin));
    Expect(TokenKind::Semicolon, "after the emit");
    return Make(SyntaxKind::Emit, mark, begin);
}

// "break" ";" | "continue" ";"
uint32 SyntaxTreeBuilder::ParseJump() noexcept {
    const uint32 begin = CurrentBegin();
    const SyntaxKind kind = At(TokenKind::Break) ? SyntaxKind::Break : SyntaxKind::Continue;
    const uint32 token = Advance();
    const usize mark = Mark();
    Expect(TokenKind::Semicolon, kind == SyntaxKind::Break ? StringView("after 'break'")
                                                           : StringView("after 'continue'"));
    return Make(kind, mark, begin, token);
}

} // namespace rocks::sparks::detail
