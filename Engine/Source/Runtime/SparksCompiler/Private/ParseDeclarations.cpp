#include "SyntaxTreeBuilder.h"

// 파일·import·선언·멤버·enum·매개변수·attribute·문서 주석의 구문 분석이다(3.6절 File·Import·Decl·
// Member·EnumDecl·Params, 8.1절 "문법 세부").

namespace rocks::sparks::detail {

// File = { Import } { Decl }. 선언 뒤의 import는 S0210을 내고 트리에 넣지 않는다.
uint32 SyntaxTreeBuilder::ParseFileNode() noexcept {
    const usize mark = Mark();
    const usize importsMark = Mark();
    const uint32 importsBegin = RawBegin();
    // import 단계: 첫 선언(선언을 시작하는 토큰) 전까지다. 그 사이의 잡토큰은 S0203이며 Error
    // 노드로 import List에 남는다(뒤의 import가 S0210이 되지 않게).
    while (error_ == ErrorCode::None) {
        const TokenKind kind = PeekKind(0);
        if (kind == TokenKind::EndOfFile ||
            (kind != TokenKind::Import && IsDeclarationStart(kind))) {
            break;
        }

        const usize before = position_;
        const usize consumed = consumed_;
        Push(kind == TokenKind::Import ? ParseImport() : ParseDeclaration());
        Recover(SyncLevel::Declaration, consumed);
        if (position_ == before) {
            SkipOne();
        }
    }

    Push(Make(SyntaxKind::List, importsMark, importsBegin));
    const usize declarationsMark = Mark();
    const uint32 declarationsBegin = RawBegin();
    while (PeekKind(0) != TokenKind::EndOfFile && error_ == ErrorCode::None) {
        const usize before = position_;
        const usize consumed = consumed_;
        if (PeekKind(0) == TokenKind::Import) {
            ReportInPlace(DiagnosticCode::ImportAfterDeclaration, CurrentSpan(),
                          "Imports must come before the declarations.",
                          "All imports of a file are listed at its top.",
                          "Move this import to the beginning of the file.");
            const NodeCheckpoint checkpoint = SaveNodes();
            static_cast<void>(ParseImport());
            RestoreNodes(checkpoint);
        } else {
            Push(ParseDeclaration());
        }

        Recover(SyncLevel::Declaration, consumed);
        if (position_ == before) {
            SkipOne();
        }
    }

    static_cast<void>(Kind()); // 파일 끝 앞의 문서 주석도 S0208이다.
    Push(Make(SyntaxKind::List, declarationsMark, declarationsBegin));
    const uint32 end = TokenAt(tree_.tokens_.Size() - 1).span.end;
    return MakeSpan(SyntaxKind::File, mark, SourceSpan{sources_.Get(id_).ContentBegin(), end},
                    NoToken, 0, 0);
}

// Import = "import" Path [ "as" Ident ] ";"
uint32 SyntaxTreeBuilder::ParseImport() noexcept {
    const uint32 begin = CurrentBegin();
    Advance();
    const usize mark = Mark();
    Push(ParsePath("after 'import'"));
    Push(Accept(TokenKind::As) ? ParseName("after 'as'") : NoNode);
    Expect(TokenKind::Semicolon, "after the import");
    return Make(SyntaxKind::Import, mark, begin);
}

uint32 SyntaxTreeBuilder::ParseName(StringView context) noexcept {
    if (At(TokenKind::Identifier)) {
        return Leaf(SyntaxKind::Name, Advance());
    }

    const bool inSync = CanReport() && !AtLexicalError();
    StringView explanation;
    if (IsKeyword(Kind())) {
        explanation = "Keywords cannot be used as names.";
    } else if (At(TokenKind::Underscore)) {
        explanation = "A lone '_' discards a value and is not a name.";
    }

    ReportExpected(DiagnosticCode::ExpectedName, "a name", context, explanation,
                   "Write a name made of English letters and digits, as in 'doorSpeed'.");
    return MakeNameError(inSync);
}

// Path = Ident { "." Ident }
uint32 SyntaxTreeBuilder::ParsePath(StringView context) noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    Push(ParseName(context));
    while (Accept(TokenKind::Dot)) {
        Push(ParseName("after '.'"));
    }

    return Make(SyntaxKind::Path, mark, begin);
}

uint32 SyntaxTreeBuilder::CollectDoc() noexcept {
    // 코드 뒤 같은 줄의 `///`는 다음 선언을 설명하지 않는다(앞 코드를 설명하려던 것일 수 있다).
    if (TokenAt(position_).kind == TokenKind::DocComment &&
        !StartsLine(TokenAt(position_).span.begin)) {
        ReportInPlace(DiagnosticCode::MisplacedDocComment, TokenAt(position_).span,
                      "A documentation comment must start its own line.",
                      "It documents the declaration, member or enum case on the following lines, "
                      "not the code before it.",
                      "Move it to its own line before the declaration, or use '//'.");
        ++position_;
    }

    if (TokenAt(position_).kind != TokenKind::DocComment) {
        return NoNode;
    }

    const usize first = position_;
    uint32 count = 0;
    while (TokenAt(position_).kind == TokenKind::DocComment) {
        ++count;
        ++position_;
    }

    const SourceSpan span{TokenAt(first).span.begin, TokenAt(position_ - 1).span.end};
    return MakeSpan(SyntaxKind::DocComment, Mark(), span, static_cast<uint32>(first), 0, count);
}

uint32 SyntaxTreeBuilder::ParseAttributes() noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    while (At(TokenKind::At)) {
        Push(ParseAttribute());
    }

    return Make(SyntaxKind::List, mark, begin);
}

// Attribute = "@" Ident [ "(" AttrArg { "," AttrArg } ")" ]. 이름은 `@` 바로 뒤에 붙는다(S0207).
uint32 SyntaxTreeBuilder::ParseAttribute() noexcept {
    const uint32 begin = CurrentBegin();
    const uint32 at = Advance();
    const usize mark = Mark();
    uint32 name = NoToken;
    if (At(TokenKind::Identifier)) {
        if (CurrentBegin() != TokenAt(at).span.end) {
            ReportInPlace(DiagnosticCode::ExpectedName, CurrentSpan(),
                          "An attribute name must follow '@' directly.",
                          "A space after '@' separates it from the name.",
                          "Remove the space, as in '@edit'.");
        }

        name = Advance();
    } else {
        ReportExpected(DiagnosticCode::ExpectedName, "an attribute name", "after '@'",
                       IsKeyword(Kind()) ? StringView("Keywords cannot be used as names.")
                                         : StringView(),
                       "Write the name right after '@', as in '@edit'.");
    }

    if (At(TokenKind::LeftParen)) {
        Push(ParseArguments());
    } else {
        Push(MakeSpan(SyntaxKind::List, Mark(), SourceSpan{previousEnd_, previousEnd_}, NoToken, 0,
                      0));
    }

    return Make(SyntaxKind::Attribute, mark, begin, name);
}

void SyntaxTreeBuilder::DiscardAttributes(StringView message) noexcept {
    const NodeCheckpoint checkpoint = SaveNodes();
    const uint32 attributes = ParseAttributes();
    ReportInPlace(DiagnosticCode::MisplacedAttribute, NodeSpan(attributes), message,
                  "Attributes go on top-level declarations, class and struct members and enum "
                  "cases.",
                  "Remove the attribute.");
    RestoreNodes(checkpoint);
}

// Decl = { Attribute } ( ClassDecl | StructDecl | EnumDecl | ExtEventDecl | ConstDecl | FnDecl |
// InterfaceDecl ). 문서 주석은 attribute보다 앞에 온다.
uint32 SyntaxTreeBuilder::ParseDeclaration() noexcept {
    const NodeCheckpoint checkpoint = SaveNodes();
    const uint32 begin = RawBegin();
    const uint32 doc = CollectDoc();
    const uint32 attributes = ParseAttributes();
    switch (Kind()) {
    case TokenKind::Class:
        return ParseClass(begin, doc, attributes, 0);
    case TokenKind::Open:
        if (PeekKind(1) == TokenKind::Class) {
            Advance();
            return ParseClass(begin, doc, attributes, SyntaxFlags::Open);
        }

        return ParseFunction(begin, doc, attributes);
    case TokenKind::Override:
    case TokenKind::Async:
    case TokenKind::Fn:
        return ParseFunction(begin, doc, attributes);
    case TokenKind::Struct:
        return ParseStruct(begin, doc, attributes);
    case TokenKind::Enum:
        return ParseEnum(begin, doc, attributes);
    case TokenKind::Interface:
        return ParseInterface(begin, doc, attributes);
    case TokenKind::Const:
        return ParseConst(begin, doc, attributes);
    case TokenKind::Event:
        return ParseExtensionEvent(begin, doc, attributes);
    default:
        break;
    }

    if (At(TokenKind::Semicolon)) {
        ReportInPlace(DiagnosticCode::ExpectedDeclaration, CurrentSpan(),
                      "A ';' cannot appear between declarations.",
                      "Declarations end with '}' or with their own ';'.", "Remove the extra ';'.");
    } else {
        ReportExpected(DiagnosticCode::ExpectedDeclaration, "a declaration", StringView(),
                       "Top-level code is made of declarations; statements go inside functions.",
                       "Start a declaration with class, struct, enum, interface, event, const or "
                       "fn.");
    }

    RestoreNodes(checkpoint);
    return SkipErrorToken(true);
}

// ClassDecl = [ "open" ] "class" Ident ":" Path { "," Path } "{" { Member } "}"
uint32 SyntaxTreeBuilder::ParseClass(uint32 begin, uint32 doc, uint32 attributes,
                                     uint32 flags) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'class'"));
    const bool hasBases = Expect(TokenKind::Colon, "after the class name",
                                 "Name the base type, as in 'class Door : ScriptableComponent'.");
    const uint32 basesBegin = CurrentBegin();
    const usize basesMark = Mark();
    if (hasBases) {
        do {
            Push(ParsePath("as a base type"));
            if (At(TokenKind::Less) || At(TokenKind::Question)) {
                ReportInPlace(DiagnosticCode::ExpectedToken, CurrentSpan(),
                              "Base types are written without type arguments or '?'.",
                              "A class extends a plain engine type or Sparks class and implements "
                              "plain interfaces.",
                              "Remove the type arguments or '?'.");
                const NodeCheckpoint checkpoint = SaveNodes();
                if (At(TokenKind::Less)) {
                    static_cast<void>(ParseTypeArguments());
                }

                Accept(TokenKind::Question);
                RestoreNodes(checkpoint);
            }
        } while (Accept(TokenKind::Comma));
    }

    Push(Make(SyntaxKind::List, basesMark, basesBegin));
    Push(ParseMemberBody(MemberContext::Class, "to start the class body",
                         "to close the class body"));
    return Make(SyntaxKind::Class, mark, begin, NoToken, flags);
}

// StructDecl = "struct" Ident "{" { { Attribute } ( FieldDecl | FnDecl ) } "}"
uint32 SyntaxTreeBuilder::ParseStruct(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'struct'"));
    Push(ParseMemberBody(MemberContext::Struct, "to start the struct body",
                         "to close the struct body"));
    return Make(SyntaxKind::Struct, mark, begin);
}

// InterfaceDecl = "interface" Ident "{" { 메서드 시그니처 } "}"
uint32 SyntaxTreeBuilder::ParseInterface(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'interface'"));
    Push(ParseMemberBody(MemberContext::Interface, "to start the interface body",
                         "to close the interface body"));
    return Make(SyntaxKind::Interface, mark, begin);
}

// `{ 멤버... }`의 멤버 List다(범위는 중괄호까지).
uint32 SyntaxTreeBuilder::ParseMemberBody(MemberContext context, StringView openContext,
                                          StringView closeContext) noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    if (!Expect(TokenKind::LeftBrace, openContext)) {
        return Make(SyntaxKind::List, mark, begin);
    }

    panic_ = false;
    while (PeekKind(0) != TokenKind::RightBrace && PeekKind(0) != TokenKind::EndOfFile &&
           error_ == ErrorCode::None) {
        const usize before = position_;
        const usize consumed = consumed_;
        Push(ParseMember(context));
        Recover(SyncLevel::Member, consumed);
        if (position_ == before) {
            SkipOne();
        }
    }

    Expect(TokenKind::RightBrace, closeContext);
    return Make(SyntaxKind::List, mark, begin);
}

uint32 SyntaxTreeBuilder::ParseMember(MemberContext context) noexcept {
    const NodeCheckpoint checkpoint = SaveNodes();
    const uint32 begin = RawBegin();
    const uint32 doc = CollectDoc();
    const uint32 attributes = ParseAttributes();
    const TokenKind kind = Kind();
    if (context == MemberContext::Interface) {
        if (kind == TokenKind::Fn || kind == TokenKind::Async) {
            if (ChildCount(attributes) != 0) {
                ReportInPlace(DiagnosticCode::MisplacedAttribute, NodeSpan(attributes),
                              "Interface methods cannot have attributes.",
                              "Attributes go on top-level declarations, class and struct members "
                              "and enum cases.",
                              "Remove the attribute.");
            }

            return ParseInterfaceMethod(begin, doc, attributes);
        }

        ReportExpected(DiagnosticCode::ExpectedMember, "an interface method", StringView(),
                       "An interface lists method signatures only.",
                       "Write a signature such as 'fn Use(user: Actor): bool;'.");
        RestoreNodes(checkpoint);
        return SkipErrorToken(false);
    }

    switch (kind) {
    case TokenKind::Var:
    case TokenKind::Let:
        return ParseField(begin, doc, attributes);
    case TokenKind::Fn:
    case TokenKind::Async:
    case TokenKind::Override:
    case TokenKind::Open:
        return ParseFunction(begin, doc, attributes);
    case TokenKind::Event:
        if (context == MemberContext::Class) {
            return ParseEvent(begin, doc, attributes);
        }

        break;
    case TokenKind::On:
        if (context == MemberContext::Class) {
            return ParseHandler(begin, doc, attributes);
        }

        break;
    default:
        break;
    }

    if (context == MemberContext::Struct) {
        ReportExpected(DiagnosticCode::ExpectedMember, "a struct member", StringView(),
                       "A struct holds fields and functions.",
                       "Start the member with var, let or fn.");
    } else {
        ReportExpected(DiagnosticCode::ExpectedMember, "a class member", StringView(),
                       "A class holds fields, functions, events and handlers.",
                       "Start the member with var, let, fn, event or on.");
    }

    RestoreNodes(checkpoint);
    return SkipErrorToken(false);
}

// FieldDecl = ( "var" | "let" ) Ident [ ":" Type ] [ "=" Expr ] ";"
uint32 SyntaxTreeBuilder::ParseField(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    const uint32 flags = At(TokenKind::Let) ? SyntaxFlags::Let : 0;
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("for the field"));
    Push(Accept(TokenKind::Colon) ? ParseType("for the field") : NoNode);
    Push(Accept(TokenKind::Equal) ? ParseExpression() : NoNode);
    Expect(TokenKind::Semicolon, "after the field");
    return Make(SyntaxKind::Field, mark, begin, NoToken, flags);
}

// FnDecl = [ "open" | "override" ] [ "async" ] "fn" Ident "(" [ Params ] ")" [ ":" Type ] Block
uint32 SyntaxTreeBuilder::ParseFunction(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    uint32 flags = 0;
    if (Accept(TokenKind::Open)) {
        flags |= SyntaxFlags::Open;
    } else if (Accept(TokenKind::Override)) {
        flags |= SyntaxFlags::Override;
    }

    if (Accept(TokenKind::Async)) {
        flags |= SyntaxFlags::Async;
    }

    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Expect(TokenKind::Fn, "after the function modifiers",
           "Write the modifiers in this order: 'open' or 'override', then 'async', then 'fn'.");
    Push(ParseName("for the function"));
    Push(ParseParameters());
    Push(Accept(TokenKind::Colon) ? ParseType("as the return type") : NoNode);
    Push(ParseBlock("to start the function body",
                    "Write the body in braces, as in 'fn Open() { ... }'."));
    return Make(SyntaxKind::Function, mark, begin, NoToken, flags);
}

// [ "async" ] "fn" Ident "(" [ Params ] ")" [ ":" Type ] ";"
uint32 SyntaxTreeBuilder::ParseInterfaceMethod(uint32 begin, uint32 doc,
                                               uint32 attributes) noexcept {
    const uint32 flags = Accept(TokenKind::Async) ? SyntaxFlags::Async : 0;
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Expect(TokenKind::Fn, "after 'async'");
    Push(ParseName("for the method"));
    Push(ParseParameters());
    Push(Accept(TokenKind::Colon) ? ParseType("as the return type") : NoNode);
    Expect(TokenKind::Semicolon, "after the interface method",
           "End the signature with ';'; interface methods have no body.");
    return Make(SyntaxKind::InterfaceMethod, mark, begin, NoToken, flags);
}

// EventDecl = "event" Ident "(" [ Params ] ")" ";"
uint32 SyntaxTreeBuilder::ParseEvent(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'event'"));
    Push(ParseParameters());
    Expect(TokenKind::Semicolon, "after the event");
    return Make(SyntaxKind::Event, mark, begin);
}

// ExtEventDecl = "event" Ident "." Ident "(" [ Params ] ")" ";"
uint32 SyntaxTreeBuilder::ParseExtensionEvent(uint32 begin, uint32 doc,
                                              uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'event'"));
    Push(Expect(TokenKind::Dot, "between the type and the event name",
                "A top-level event extends a type, as in 'event Actor.Interacted(by: Actor);'.")
             ? ParseName("for the event")
             : MakeError());
    Push(ParseParameters());
    Expect(TokenKind::Semicolon, "after the event");
    return Make(SyntaxKind::ExtensionEvent, mark, begin);
}

// Handler = "on" EvPath "(" [ Params ] ")" Block
uint32 SyntaxTreeBuilder::ParseHandler(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseEventPath("after 'on'"));
    Push(ParseParameters());
    Push(ParseBlock("to start the handler body",
                    "Write the body in braces, as in 'on Opened() { ... }'."));
    return Make(SyntaxKind::Handler, mark, begin);
}

// EvPath = ( Ident | "this" ) { "." Ident }
uint32 SyntaxTreeBuilder::ParseEventPath(StringView context) noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    Push(At(TokenKind::This) ? Leaf(SyntaxKind::This, Advance()) : ParseName(context));
    while (Accept(TokenKind::Dot)) {
        Push(ParseName("after '.'"));
    }

    return Make(SyntaxKind::EventPath, mark, begin);
}

// ConstDecl = "const" Ident ":" Type "=" Expr ";"
uint32 SyntaxTreeBuilder::ParseConst(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    Push(ParseName("after 'const'"));
    Push(Expect(TokenKind::Colon, "after the constant name",
                "State the constant's type, as in 'const MaxStack: int = 99;'.")
             ? ParseType("for the constant")
             : MakeError());
    Push(Expect(TokenKind::Equal, "after the constant type",
                "Give the constant a value, as in 'const MaxStack: int = 99;'.")
             ? ParseExpression()
             : MakeError());
    Expect(TokenKind::Semicolon, "after the constant");
    return Make(SyntaxKind::Const, mark, begin);
}

// EnumDecl = "enum" Ident "{" EnumCase { "," EnumCase } [ "," ] "}". case가 없으면 S0219다.
uint32 SyntaxTreeBuilder::ParseEnum(uint32 begin, uint32 doc, uint32 attributes) noexcept {
    Advance();
    const usize mark = Mark();
    Push(doc);
    Push(attributes);
    const uint32 name = ParseName("after 'enum'");
    Push(name);
    const uint32 casesBegin = CurrentBegin();
    const usize casesMark = Mark();
    if (Expect(TokenKind::LeftBrace, "to start the enum body")) {
        panic_ = false;
        // 이름이 빠진 enum은 그 진단(S0207) 하나로 둔다.
        if (PeekKind(0) == TokenKind::RightBrace && NodeKind(name) == SyntaxKind::Name) {
            ReportInPlace(DiagnosticCode::EmptyEnum, NodeSpan(name),
                          "An enum needs at least one case.",
                          "The zero value of an enum is its first case.",
                          "Add a case, as in 'enum DoorState { Closed, Open }'.");
        }

        while (PeekKind(0) != TokenKind::RightBrace && PeekKind(0) != TokenKind::EndOfFile &&
               error_ == ErrorCode::None) {
            const usize before = position_;
            Push(ParseEnumCase());
            if (Accept(TokenKind::Comma)) {
                panic_ = false; // `,`는 enum 본문의 동기화 지점이다.
            } else if (PeekKind(0) != TokenKind::RightBrace) {
                Expect(TokenKind::Comma, "between enum cases");
                while (PeekKind(0) != TokenKind::EndOfFile && PeekKind(0) != TokenKind::Comma &&
                       PeekKind(0) != TokenKind::RightBrace) {
                    if (PeekKind(0) == TokenKind::LeftBrace) {
                        SkipBalancedBraces();
                    } else {
                        SkipOne();
                    }
                }

                if (PeekKind(0) == TokenKind::Comma) {
                    SkipOne();
                }

                if (PeekKind(0) != TokenKind::EndOfFile) {
                    panic_ = false;
                }
            }

            if (position_ == before) {
                SkipOne();
            }
        }

        Expect(TokenKind::RightBrace, "to close the enum body");
    }

    Push(Make(SyntaxKind::List, casesMark, casesBegin));
    return Make(SyntaxKind::Enum, mark, begin);
}

// EnumCase = { Attribute } Ident (문서 주석은 그 앞)
uint32 SyntaxTreeBuilder::ParseEnumCase() noexcept {
    const uint32 begin = RawBegin();
    const usize mark = Mark();
    Push(CollectDoc());
    Push(ParseAttributes());
    Push(ParseName("for the enum case"));
    return Make(SyntaxKind::EnumCase, mark, begin);
}

// "(" [ Params ] ")"의 Parameter List다(범위는 괄호까지).
uint32 SyntaxTreeBuilder::ParseParameters() noexcept {
    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    if (Expect(TokenKind::LeftParen, "to start the parameter list")) {
        if (!At(TokenKind::RightParen)) {
            do {
                Push(ParseParameter());
            } while (Accept(TokenKind::Comma));
        }

        Expect(TokenKind::RightParen, "to close the parameter list");
    }

    return Make(SyntaxKind::List, mark, begin);
}

// Param = ( Ident | "_" ) ":" Type
uint32 SyntaxTreeBuilder::ParseParameter() noexcept {
    if (At(TokenKind::At)) {
        DiscardAttributes("Parameters cannot have attributes.");
        // 뒤에 매개변수 이름이 없으면 S0214 하나로 둔다.
        if (!At(TokenKind::Identifier) && !At(TokenKind::Underscore)) {
            panic_ = true;
        }
    }

    const uint32 begin = CurrentBegin();
    const usize mark = Mark();
    Push(At(TokenKind::Underscore) ? Leaf(SyntaxKind::Discard, Advance())
                                   : ParseName("for the parameter"));
    Push(Expect(TokenKind::Colon, "after the parameter name",
                "State the parameter type, as in 'amount: int'.")
             ? ParseType("for the parameter")
             : MakeError());
    return Make(SyntaxKind::Parameter, mark, begin);
}

} // namespace rocks::sparks::detail
