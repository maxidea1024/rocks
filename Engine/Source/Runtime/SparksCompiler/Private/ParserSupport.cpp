#include "SyntaxTreeBuilder.h"

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Lexer.h>

// 구문 분석기의 바탕이다: 토큰 분류·토큰 읽기·노드 만들기·진단·복구·깊이 상한(SyntaxTreeBuilder.h
// 설명, Docs/Design/SparksLanguage 8.1절 "복구").

namespace rocks::sparks::detail {

namespace {

// 진단 문장을 담는 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize ParserMessageInlineBytes = 192;

// "found identifier 'x'"에 그대로 보이는 식별자의 최대 byte 수다. 넘으면 앞부분 뒤에 "..."를
// 붙인다.
constexpr usize MaxQuotedIdentifierBytes = 48;

} // namespace

SyntaxTreeBuilder::SyntaxTreeBuilder(const SourceSet& sources, SourceId id,
                                     DiagnosticList& diagnostics, SyntaxTree& tree) noexcept
    : sources_(sources), id_(id), diagnostics_(diagnostics), tree_(tree),
      allocator_(tree.nodes_.GetAllocator()), stack_(allocator_), depths_(allocator_),
      lexicalErrors_(allocator_) {
}

Result<void> SyntaxTreeBuilder::Run() noexcept {
    tree_.nodes_.Clear();
    tree_.children_.Clear();
    tree_.tokens_.Clear();
    tree_.source_ = id_;
    tree_.root_ = NoNode;
    const usize lexicalBegin = diagnostics_.Count();
    if (auto lexed = Tokenize(sources_, id_, diagnostics_, tree_.tokens_); !lexed) {
        return lexed;
    }

    for (usize index = lexicalBegin; index < diagnostics_.Count(); ++index) {
        if (!diagnostics_[index].HasSpan()) {
            continue;
        }

        if (auto pushed = lexicalErrors_.TryPushBack(diagnostics_[index].Span().begin); !pushed) {
            return pushed;
        }
    }

    algo::Sort(lexicalErrors_.View());

    const uint32 root = ParseFileNode();
    if (error_ == ErrorCode::None) {
        CheckTreeDepth(root);
    }

    if (error_ != ErrorCode::None) {
        return Result<void>::Failure(error_);
    }

    tree_.root_ = root;
    return Result<void>::Success();
}

// ---- 토큰 분류 ----

bool SyntaxTreeBuilder::IsStatementKeyword(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Var:
    case TokenKind::Let:
    case TokenKind::If:
    case TokenKind::For:
    case TokenKind::While:
    case TokenKind::Switch:
    case TokenKind::Return:
    case TokenKind::Emit:
    case TokenKind::Break:
    case TokenKind::Continue:
    case TokenKind::Try:
    case TokenKind::Await:
        return true;
    default:
        return false;
    }
}

bool SyntaxTreeBuilder::IsMemberStart(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Var:
    case TokenKind::Let:
    case TokenKind::Fn:
    case TokenKind::Async:
    case TokenKind::Override:
    case TokenKind::Open:
    case TokenKind::Event:
    case TokenKind::On:
    case TokenKind::At:
        return true;
    default:
        return false;
    }
}

bool SyntaxTreeBuilder::IsDeclarationStart(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Import:
    case TokenKind::Class:
    case TokenKind::Struct:
    case TokenKind::Enum:
    case TokenKind::Interface:
    case TokenKind::Event:
    case TokenKind::Const:
    case TokenKind::Fn:
    case TokenKind::Async:
    case TokenKind::Override:
    case TokenKind::Open:
    case TokenKind::At:
        return true;
    default:
        return false;
    }
}

bool SyntaxTreeBuilder::StartsValue(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::Identifier:
    case TokenKind::IntegerLiteral:
    case TokenKind::FloatLiteral:
    case TokenKind::StringBegin:
    case TokenKind::VerbatimString:
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::Null:
    case TokenKind::This:
    case TokenKind::LeftParen:
    case TokenKind::LeftBracket:
    case TokenKind::Bang:
    case TokenKind::Minus:
    case TokenKind::Try:
    case TokenKind::Await:
        return true;
    default:
        return false;
    }
}

bool SyntaxTreeBuilder::IsComparison(TokenKind kind) noexcept {
    return kind == TokenKind::Less || kind == TokenKind::LessEqual || kind == TokenKind::Greater ||
           kind == TokenKind::GreaterEqual;
}

bool SyntaxTreeBuilder::IsEquality(TokenKind kind) noexcept {
    return kind == TokenKind::EqualEqual || kind == TokenKind::BangEqual;
}

bool SyntaxTreeBuilder::IsCompoundAssignment(TokenKind kind) noexcept {
    return kind == TokenKind::PlusEqual || kind == TokenKind::MinusEqual ||
           kind == TokenKind::StarEqual || kind == TokenKind::SlashEqual;
}

bool SyntaxTreeBuilder::IsExitKeyword(TokenKind kind) noexcept {
    return kind == TokenKind::Return || kind == TokenKind::Break || kind == TokenKind::Continue;
}

// 대입의 왼쪽이 될 수 있는 모양이다(S0211). 이름·멤버·원소 경로만이며, 그 경로 안의 호출 결과·`?.`·
// `this.`는 뒤 단계가 판정한다(S0731).
bool SyntaxTreeBuilder::IsAssignable(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::Identifier || kind == SyntaxKind::Member ||
           kind == SyntaxKind::Index;
}

// ---- 토큰 읽기 ----

const Token& SyntaxTreeBuilder::TokenAt(usize index) const noexcept {
    return tree_.tokens_[index];
}

usize SyntaxTreeBuilder::SkipDocs(usize index) const noexcept {
    while (TokenAt(index).kind == TokenKind::DocComment) {
        ++index;
    }

    return index;
}

TokenKind SyntaxTreeBuilder::PeekKind(usize ahead) const noexcept {
    usize index = SkipDocs(position_);
    for (usize step = 0; step < ahead && TokenAt(index).kind != TokenKind::EndOfFile; ++step) {
        index = SkipDocs(index + 1);
    }

    return TokenAt(index).kind;
}

void SyntaxTreeBuilder::ReportMisplacedDocs() noexcept {
    while (TokenAt(position_).kind == TokenKind::DocComment) {
        ReportInPlace(DiagnosticCode::MisplacedDocComment, TokenAt(position_).span,
                      "Documentation comment is not attached to a declaration.",
                      "'///' documents the declaration, member or enum case right after it and "
                      "goes before its attributes.",
                      "Use '//' for an ordinary comment.");
        ++position_;
    }
}

TokenKind SyntaxTreeBuilder::Kind() noexcept {
    ReportMisplacedDocs();
    return TokenAt(position_).kind;
}

bool SyntaxTreeBuilder::At(TokenKind kind) noexcept {
    return Kind() == kind;
}

uint32 SyntaxTreeBuilder::RawBegin() const noexcept {
    return TokenAt(position_).span.begin;
}

uint32 SyntaxTreeBuilder::CurrentBegin() noexcept {
    ReportMisplacedDocs();
    return TokenAt(position_).span.begin;
}

SourceSpan SyntaxTreeBuilder::CurrentSpan() noexcept {
    ReportMisplacedDocs();
    return TokenAt(position_).span;
}

uint32 SyntaxTreeBuilder::Advance() noexcept {
    ReportMisplacedDocs();
    const usize index = position_;
    const Token& token = TokenAt(index);
    if (token.kind != TokenKind::EndOfFile) {
        ++position_;
        ++consumed_;
        previousEnd_ = token.span.end;
        lastKind_ = token.kind;
    }

    return static_cast<uint32>(index);
}

bool SyntaxTreeBuilder::Accept(TokenKind kind) noexcept {
    if (!At(kind)) {
        return false;
    }

    Advance();
    return true;
}

void SyntaxTreeBuilder::SkipOne() noexcept {
    position_ = SkipDocs(position_);
    const Token& token = TokenAt(position_);
    if (token.kind != TokenKind::EndOfFile) {
        ++position_;
        ++consumed_;
        previousEnd_ = token.span.end;
        lastKind_ = token.kind;
    }
}

void SyntaxTreeBuilder::SkipBalancedBraces() noexcept {
    usize depth = 0;
    while (PeekKind(0) != TokenKind::EndOfFile) {
        const TokenKind kind = PeekKind(0);
        SkipOne();
        if (kind == TokenKind::LeftBrace) {
            ++depth;
        } else if (kind == TokenKind::RightBrace) {
            if (depth <= 1) {
                return;
            }

            --depth;
        }
    }
}

// ---- 노드 ----

usize SyntaxTreeBuilder::Mark() const noexcept {
    return stack_.Size();
}

void SyntaxTreeBuilder::Push(uint32 node) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    if (auto pushed = stack_.TryPushBack(node); !pushed) {
        error_ = pushed.Error();
    }
}

uint32 SyntaxTreeBuilder::MakeSpan(SyntaxKind kind, usize mark, SourceSpan span, uint32 token,
                                   uint32 flags, uint32 extra) noexcept {
    if (error_ != ErrorCode::None) {
        if (mark < stack_.Size()) {
            static_cast<void>(stack_.TryResize(mark)); // 축소는 실패하지 않는다.
        }

        return NoNode;
    }

    uint32 depth = 1;
    for (usize index = mark; index < stack_.Size(); ++index) {
        const uint32 child = stack_[index];
        if (child != NoNode && depths_[child] + 1u > depth) {
            depth = depths_[child] + 1u;
        }
    }

    SyntaxNode node;
    node.kind = kind;
    node.flags = flags;
    node.token = token;
    node.span = span;
    node.firstChild = static_cast<uint32>(tree_.children_.Size());
    node.childCount = static_cast<uint32>(stack_.Size() - mark);
    node.extra = extra;
    for (usize index = mark; index < stack_.Size(); ++index) {
        if (auto pushed = tree_.children_.TryPushBack(stack_[index]); !pushed) {
            error_ = pushed.Error();
            return NoNode;
        }
    }

    static_cast<void>(stack_.TryResize(mark)); // 축소는 실패하지 않는다.
    if (auto pushed = tree_.nodes_.TryPushBack(node); !pushed) {
        error_ = pushed.Error();
        return NoNode;
    }

    // 노드 수는 토큰 수(원문 상한 16 MiB) 이하이므로 깊이는 uint32에 넉넉히 든다.
    if (auto pushed = depths_.TryPushBack(depth); !pushed) {
        error_ = pushed.Error();
        return NoNode;
    }

    return static_cast<uint32>(tree_.nodes_.Size() - 1);
}

uint32 SyntaxTreeBuilder::Make(SyntaxKind kind, usize mark, uint32 begin, uint32 token,
                               uint32 flags) noexcept {
    uint32 end = previousEnd_ > begin ? previousEnd_ : begin;
    if (error_ == ErrorCode::None) {
        // 복구로 만든 Error 노드는 아직 읽지 않은 토큰 위치에 있을 수 있다.
        for (usize index = mark; index < stack_.Size(); ++index) {
            const uint32 child = stack_[index];
            if (child != NoNode && tree_.nodes_[child].span.end > end) {
                end = tree_.nodes_[child].span.end;
            }
        }
    }

    return MakeSpan(kind, mark, SourceSpan{begin, end}, token, flags, 0);
}

uint32 SyntaxTreeBuilder::Leaf(SyntaxKind kind, uint32 token) noexcept {
    return MakeSpan(kind, Mark(), TokenAt(token).span, token, 0, 0);
}

uint32 SyntaxTreeBuilder::MakeError() noexcept {
    const uint32 begin = TokenAt(SkipDocs(position_)).span.begin;
    return MakeSpan(SyntaxKind::Error, Mark(), SourceSpan{begin, begin}, NoToken, 0, 0);
}

bool SyntaxTreeBuilder::AtMistakenKeyword(bool inSync) const noexcept {
    const Token& token = TokenAt(SkipDocs(position_));
    if (!inSync || !IsKeyword(token.kind)) {
        return false;
    }

    const StringView text = sources_.Get(id_).Text();
    for (uint32 offset = previousEnd_; offset < token.span.begin; ++offset) {
        if (text.Data()[offset] == '\n') {
            return false;
        }
    }

    return true;
}

uint32 SyntaxTreeBuilder::MakeNameError(bool inSync) noexcept {
    return AtMistakenKeyword(inSync) ? Leaf(SyntaxKind::Error, Advance()) : MakeError();
}

bool SyntaxTreeBuilder::StartsLine(uint32 offset) const noexcept {
    const SourceFile& file = sources_.Get(id_);
    const StringView text = file.Text();
    while (offset > file.ContentBegin()) {
        const char previous = text.Data()[offset - 1];
        if (previous == '\n') {
            return true;
        }

        if (previous != ' ' && previous != '\t') {
            return false;
        }

        --offset;
    }

    return true;
}

SyntaxTreeBuilder::NodeCheckpoint SyntaxTreeBuilder::SaveNodes() const noexcept {
    NodeCheckpoint checkpoint;
    checkpoint.nodes = tree_.nodes_.Size();
    checkpoint.children = tree_.children_.Size();
    return checkpoint;
}

void SyntaxTreeBuilder::RestoreNodes(NodeCheckpoint checkpoint) noexcept {
    // 축소는 실패하지 않는다. 할당 실패 뒤에는 이미 더 짧을 수 있다.
    if (checkpoint.nodes < tree_.nodes_.Size()) {
        static_cast<void>(tree_.nodes_.TryResize(checkpoint.nodes));
    }

    if (checkpoint.nodes < depths_.Size()) {
        static_cast<void>(depths_.TryResize(checkpoint.nodes));
    }

    if (checkpoint.children < tree_.children_.Size()) {
        static_cast<void>(tree_.children_.TryResize(checkpoint.children));
    }
}

uint32 SyntaxTreeBuilder::NodeBegin(uint32 node) const noexcept {
    return node == NoNode ? previousEnd_ : tree_.nodes_[node].span.begin;
}

SyntaxKind SyntaxTreeBuilder::NodeKind(uint32 node) const noexcept {
    return node == NoNode ? SyntaxKind::Error : tree_.nodes_[node].kind;
}

SourceSpan SyntaxTreeBuilder::NodeSpan(uint32 node) const noexcept {
    return node == NoNode ? SourceSpan{previousEnd_, previousEnd_} : tree_.nodes_[node].span;
}

uint32 SyntaxTreeBuilder::ChildCount(uint32 node) const noexcept {
    return node == NoNode ? 0 : tree_.nodes_[node].childCount;
}

// ---- 진단 ----

bool SyntaxTreeBuilder::CanReport() const noexcept {
    return !panic_ && !stopped_ && error_ == ErrorCode::None;
}

bool SyntaxTreeBuilder::AtLexicalError() const noexcept {
    const Token& token = TokenAt(SkipDocs(position_));
    if (token.kind == TokenKind::Invalid) {
        return true;
    }

    // lexicalErrors_는 오름차순이다.
    usize low = 0;
    usize high = lexicalErrors_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (lexicalErrors_[middle] < token.span.begin) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < lexicalErrors_.Size() && lexicalErrors_[low] == token.span.begin;
}

void SyntaxTreeBuilder::AddDiagnostic(DiagnosticCode code, SourceSpan span, StringView message,
                                      StringView explanation, StringView fix) noexcept {
    if (auto added = diagnostics_.TryAdd(code, id_, span, message, explanation, fix); !added) {
        error_ = added.Error();
    }
}

void SyntaxTreeBuilder::Report(DiagnosticCode code, SourceSpan span, StringView message,
                               StringView explanation, StringView fix) noexcept {
    if (CanReport() && !AtLexicalError() && span.begin != lastReportedBegin_) {
        AddDiagnostic(code, span, message, explanation, fix);
        lastReportedBegin_ = span.begin;
    }

    panic_ = true;
}

void SyntaxTreeBuilder::ReportAtCurrent(DiagnosticCode code, StringView message,
                                        StringView explanation, StringView fix) noexcept {
    Report(code, CurrentSpan(), message, explanation, fix);
}

void SyntaxTreeBuilder::ReportInPlace(DiagnosticCode code, SourceSpan span, StringView message,
                                      StringView explanation, StringView fix) noexcept {
    if (CanReport() && span.begin != lastReportedBegin_) {
        AddDiagnostic(code, span, message, explanation, fix);
        lastReportedBegin_ = span.begin;
    }
}

void SyntaxTreeBuilder::ReportExpected(DiagnosticCode code, StringView what, StringView context,
                                       StringView explanation, StringView fix) noexcept {
    ReportMisplacedDocs();
    if (!CanReport() || AtLexicalError()) {
        panic_ = true;
        return;
    }

    const Token& found = TokenAt(position_);
    StringBuilder<ParserMessageInlineBytes> message(allocator_);
    message.Append("Expected ").Append(what);
    if (!context.IsEmpty()) {
        message.Append(" ").Append(context);
    }

    message.Append(", found ");
    if (found.kind == TokenKind::Identifier) {
        // 식별자는 ASCII뿐이므로(3.1절) byte 단위로 잘라도 UTF-8이 깨지지 않는다.
        const StringView text(sources_.Get(id_).Text().Data() + found.span.begin,
                              found.span.end - found.span.begin);
        message.Append("identifier '");
        if (text.ByteCount() > MaxQuotedIdentifierBytes) {
            message.Append(StringView(text.Data(), MaxQuotedIdentifierBytes)).Append("...");
        } else {
            message.Append(text);
        }

        message.Append("'");
    } else {
        message.Append(TokenKindText(found.kind));
    }

    message.Append(".");
    if (message.HasFailed()) {
        error_ = message.Status().Error();
        return;
    }

    Report(code, found.span, message.View(), explanation, fix);
}

bool SyntaxTreeBuilder::Expect(TokenKind kind, StringView context, StringView fix) noexcept {
    if (At(kind)) {
        Advance();
        return true;
    }

    if (At(TokenKind::QuestionQuestion) && IsExitKeyword(PeekKind(1))) {
        ReportMisplacedExit();
        return false;
    }

    if (At(TokenKind::DotDotLess)) {
        ReportRangeOutsideFor();
        return false;
    }

    if (At(TokenKind::As)) {
        ReportAtCurrent(DiagnosticCode::ExpectedToken, "Use 'as?' for a downcast.",
                        "Sparks has only the checked downcast 'as?', which gives null when the "
                        "object is not of that type.",
                        "Write 'x as? T'.");
        return false;
    }

    StringBuilder<ParserMessageInlineBytes> insert(allocator_);
    if (fix.IsEmpty()) {
        insert.Append("Insert ").Append(TokenKindText(kind)).Append(".");
        if (insert.HasFailed()) {
            error_ = insert.Status().Error();
            return false;
        }

        fix = insert.View();
    }

    ReportExpected(DiagnosticCode::ExpectedToken, TokenKindText(kind), context, StringView(), fix);
    return false;
}

// `??`와 그 뒤 키워드는 이 오류의 일부이므로 소비한다(`return`에서 다시 문장으로 읽지 않게).
void SyntaxTreeBuilder::ReportMisplacedExit() noexcept {
    if (PeekKind(1) == TokenKind::Return) {
        ReportAtCurrent(DiagnosticCode::MisplacedExit,
                        "'?? return' is only allowed after a local variable's value.",
                        "It ends the function when the value is null, so it belongs where a local "
                        "variable is declared.",
                        "Write 'let x = value ?? return;'.");
    } else {
        ReportAtCurrent(DiagnosticCode::MisplacedExit, "'?? break' and '?? continue' do not exist.",
                        "Only '?? return' leaves early when a value is null.",
                        "Test the value with 'if (let x = value) { ... }' instead.");
    }

    Advance();
    Advance();
}

void SyntaxTreeBuilder::ReportRangeOutsideFor() noexcept {
    ReportAtCurrent(DiagnosticCode::RangeOutsideFor, "A range is only allowed in a 'for' header.",
                    "'a..<b' names the values a for loop visits; it is not a value.",
                    "Write 'for (i in a..<b) { ... }'.");
}

// ---- 복구 ----

void SyntaxTreeBuilder::Recover(SyncLevel level, usize consumedBefore) noexcept {
    // 파일 끝에서는 풀지 않는다(바깥 본문들의 빠진 `}`를 다시 알리지 않게).
    if (!panic_ || stopped_ || PeekKind(0) == TokenKind::EndOfFile) {
        return;
    }

    if (consumed_ > consumedBefore &&
        (lastKind_ == TokenKind::Semicolon || lastKind_ == TokenKind::RightBrace)) {
        panic_ = false;
        return;
    }

    switch (level) {
    case SyncLevel::Statement:
        SyncStatement();
        break;
    case SyncLevel::Member:
        SyncMember();
        break;
    case SyncLevel::Declaration:
        SyncDeclaration();
        break;
    }
}

void SyntaxTreeBuilder::SyncStatement() noexcept {
    while (true) {
        const TokenKind kind = PeekKind(0);
        if (kind == TokenKind::EndOfFile) {
            return; // 복구 상태를 유지한다.
        }

        if (kind == TokenKind::RightBrace || IsStatementKeyword(kind)) {
            break;
        }

        if (kind == TokenKind::Semicolon) {
            SkipOne();
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

void SyntaxTreeBuilder::SyncMember() noexcept {
    while (true) {
        const TokenKind kind = PeekKind(0);
        if (kind == TokenKind::EndOfFile) {
            return;
        }

        if (kind == TokenKind::RightBrace || IsMemberStart(kind)) {
            break;
        }

        if (kind == TokenKind::Semicolon) {
            SkipOne();
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

// 최상위의 `;`·`}`는 그 자체가 오류이므로 동기화 지점이 아니다(건너뛴다).
void SyntaxTreeBuilder::SyncDeclaration() noexcept {
    while (true) {
        const TokenKind kind = PeekKind(0);
        if (kind == TokenKind::EndOfFile) {
            return;
        }

        if (IsDeclarationStart(kind)) {
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

uint32 SyntaxTreeBuilder::SkipErrorToken(bool topLevel) noexcept {
    const TokenKind kind = PeekKind(0);
    if (kind == TokenKind::EndOfFile || (kind == TokenKind::RightBrace && !topLevel)) {
        return MakeError();
    }

    const usize index = SkipDocs(position_);
    if (kind == TokenKind::LeftBrace) {
        const uint32 begin = TokenAt(index).span.begin;
        SkipBalancedBraces();
        return MakeSpan(SyntaxKind::Error, Mark(), SourceSpan{begin, previousEnd_}, NoToken, 0, 0);
    }

    SkipOne();
    // 오류로 건너뛴 `;`·`}`는 동기화 지점이 아니다(최상위 `} x`가 진단 둘이 되지 않게).
    lastKind_ = TokenKind::Invalid;
    return Leaf(SyntaxKind::Error, static_cast<uint32>(index));
}

void SyntaxTreeBuilder::StopTooDeep(SourceSpan span) noexcept {
    if (stopped_) {
        return;
    }

    if (error_ == ErrorCode::None) {
        StringBuilder<ParserMessageInlineBytes> explanation(allocator_);
        explanation.AppendFormat(
            "Each statement, expression (including those inside parentheses, brackets, "
            "arguments, indexes and interpolations), unary operator, '?"
            "?', 'try', 'await', type and 'else if' adds a level; at most {} levels are "
            "allowed and the rest of the file is not analyzed.",
            MaxSyntaxNesting);
        if (explanation.HasFailed()) {
            error_ = explanation.Status().Error();
        } else {
            AddDiagnostic(DiagnosticCode::NestingTooDeep, span, "Code is nested too deeply.",
                          explanation.View(), "Split the code into local variables or functions.");
        }
    }

    stopped_ = true;
    position_ = tree_.tokens_.Size() - 1;
}

void SyntaxTreeBuilder::CheckTreeDepth(uint32 root) noexcept {
    if (root == NoNode || depths_[root] <= MaxSyntaxTreeDepth) {
        return;
    }

    // 가장 깊은 경로를 따라 MaxSyntaxTreeDepth + 1 단계(뿌리가 1단계)까지 내려간다. 가장 깊은
    // 자식이 여럿이면 자식 순서에서 앞의 것이다.
    uint32 node = root;
    for (uint32 level = 1; level <= MaxSyntaxTreeDepth; ++level) {
        const SyntaxNode& current = tree_.nodes_[node];
        uint32 next = NoNode;
        for (uint32 index = 0; index < current.childCount; ++index) {
            const uint32 child = tree_.children_[current.firstChild + index];
            if (child != NoNode && depths_[child] + 1u == depths_[node]) {
                next = child;
                break;
            }
        }

        node = next;
    }

    StringBuilder<ParserMessageInlineBytes> explanation(allocator_);
    explanation.AppendFormat("Code is at most {} levels deep, and each operator in a chain such as "
                             "'a + b + c' or 'a.b.c' adds a level.",
                             MaxSyntaxTreeDepth);
    if (explanation.HasFailed()) {
        error_ = explanation.Status().Error();
        return;
    }

    AddDiagnostic(DiagnosticCode::NestingTooDeep, tree_.nodes_[node].span,
                  "Code is nested too deeply.", explanation.View(),
                  "Split the code into local variables or functions.");
}

// ---- 중첩 ----

SyntaxTreeBuilder::Nesting::Nesting(SyntaxTreeBuilder& builder) noexcept : builder_(builder) {
    ++builder_.depth_;
    if (builder_.depth_ > MaxSyntaxNesting) {
        builder_.StopTooDeep(builder_.TokenAt(builder_.SkipDocs(builder_.position_)).span);
    }
}

SyntaxTreeBuilder::Nesting::~Nesting() noexcept {
    --builder_.depth_;
}

bool SyntaxTreeBuilder::Nesting::TooDeep() const noexcept {
    return builder_.stopped_;
}

} // namespace rocks::sparks::detail
