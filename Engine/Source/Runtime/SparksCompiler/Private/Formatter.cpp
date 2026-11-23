#include <SparksCompiler/Formatter.h>
#include <SparksCompiler/Lexer.h>
#include <SparksCompiler/SyntaxTree.h>

// 공식 formatter다(Docs/Design/SparksTools.ko.md 8절): 단위(토큰·문자열·주석)를 원문 순서로 걸으며
// 단위 사이의 줄바꿈·빈 줄·들여쓰기·공백만 정한다.
namespace rocks::sparks {

namespace {

// 들여쓰기 한 단계의 공백 수다.
constexpr uint32 IndentWidth = 4;

enum class UnitKind : uint8 {
    Token,
    Text, // 문자열 리터럴 전체(`"`부터 닫는 `"`까지 — 보간 포함, 원문 그대로)
    LineComment,
    BlockComment
};

// 단위의 구문 표지다(트리에서 가린다).
struct UnitFlags {
    static constexpr uint8 TypeOpen = 1u << 0;     // 타입 인자를 여는 `<`
    static constexpr uint8 TypeClose = 1u << 1;    // 타입 인자를 닫는 `>`
    static constexpr uint8 Unary = 1u << 2;        // 단항 `!`·`-`
    static constexpr uint8 AttributeEnd = 1u << 3; // attribute의 마지막 토큰
    static constexpr uint8 EnumBrace = 1u << 4;    // enum 본문을 여는 `{`
    static constexpr uint8 BaseColon = 1u << 5;    // class 기반 목록의 `:`(앞에도 한 칸)
};

struct Unit {
    UnitKind kind = UnitKind::Token;
    TokenKind token = TokenKind::EndOfFile; // Text는 StringBegin
    uint8 flags = 0;
    uint32 begin = 0;
    uint32 end = 0;
};

// 열린 `{` 하나다.
struct Brace {
    bool enumBody = false;
    uint32 brackets = 0; // 열 때의 괄호 수
};

bool IsComment(const Unit& unit) noexcept {
    return unit.kind == UnitKind::LineComment || unit.kind == UnitKind::BlockComment;
}

bool IsToken(const Unit& unit, TokenKind kind) noexcept {
    return unit.kind == UnitKind::Token && unit.token == kind;
}

bool IsCloser(const Unit& unit) noexcept {
    return IsToken(unit, TokenKind::RightParen) || IsToken(unit, TokenKind::RightBracket);
}

// 같은 줄의 두 단위 사이에 공백 한 칸이 드는지다(8.2절 "공백").
bool SpaceBetween(const Unit& left, const Unit& right) noexcept {
    if (IsComment(left) || IsComment(right)) {
        return true;
    }

    switch (right.token) {
    case TokenKind::Comma:
    case TokenKind::Semicolon:
    case TokenKind::RightParen:
    case TokenKind::RightBracket:
    case TokenKind::Dot:
    case TokenKind::QuestionDot:
    case TokenKind::DotDotLess:
    case TokenKind::Colon:
        return (right.flags & UnitFlags::BaseColon) != 0;
    case TokenKind::Question:
        return false;
    default:
        break;
    }

    if ((right.flags & (UnitFlags::TypeOpen | UnitFlags::TypeClose)) != 0) {
        return false;
    }

    switch (left.token) {
    case TokenKind::LeftParen:
    case TokenKind::LeftBracket:
    case TokenKind::Dot:
    case TokenKind::QuestionDot:
    case TokenKind::DotDotLess:
    case TokenKind::At:
        return false;
    default:
        break;
    }

    if ((left.flags & (UnitFlags::Unary | UnitFlags::TypeOpen)) != 0) {
        return false;
    }

    // 피연산자 뒤의 `(`는 호출, `[`는 색인이다(붙인다).
    const bool operand =
        left.token == TokenKind::Identifier || left.token == TokenKind::RightParen ||
        left.token == TokenKind::RightBracket || left.token == TokenKind::This ||
        left.token == TokenKind::StringBegin || left.token == TokenKind::VerbatimString;
    if (right.token == TokenKind::LeftParen) {
        return !operand && (left.flags & UnitFlags::TypeClose) == 0;
    }

    if (right.token == TokenKind::LeftBracket) {
        return !operand;
    }

    return true;
}

class Formatter final {
public:
    Formatter(Allocator& allocator, StringView text, StringBuilderBase& out) noexcept
        : text_(text), out_(out), units_(allocator), braces_(allocator), brackets_(allocator) {
    }

    Array<Unit>& Units() noexcept {
        return units_;
    }

    Result<void> Run() noexcept {
        for (usize at = 0; at < units_.Size(); ++at) {
            if (auto placed = Place(at); !placed) {
                return placed;
            }
        }

        if (!units_.IsEmpty() && !out_.TryAppend("\n")) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

private:
    // 원문 [begin, end)의 줄바꿈 수다.
    uint32 Newlines(uint32 begin, uint32 end) const noexcept {
        uint32 count = 0;
        for (uint32 at = begin; at < end; ++at) {
            count += text_.Data()[at] == '\n' ? 1 : 0;
        }

        return count;
    }

    // 앞 의미 토큰 뒤가 문장 경계인지다(이어지는 줄이 아니다).
    bool AtBoundary() const noexcept {
        if (!hasSignificant_) {
            return true;
        }

        const Unit& last = significant_;
        if (IsToken(last, TokenKind::Semicolon) || IsToken(last, TokenKind::LeftBrace) ||
            IsToken(last, TokenKind::RightBrace) || IsToken(last, TokenKind::DocComment) ||
            (last.flags & UnitFlags::AttributeEnd) != 0) {
            return true;
        }

        return IsToken(last, TokenKind::Comma) && !braces_.IsEmpty() &&
               braces_[braces_.Size() - 1].enumBody &&
               brackets_.Size() == braces_[braces_.Size() - 1].brackets;
    }

    // 단위 at 앞에서 줄을 바꾸는지와 빈 줄을 두는지다(8.2절 "줄바꿈"·"빈 줄").
    void Break(usize at, bool& newline, bool& blank) const noexcept {
        const Unit& unit = units_[at];
        const Unit& previous = units_[at - 1];
        const uint32 gap = Newlines(previous.end, unit.begin);
        if (previous.kind == UnitKind::LineComment) {
            newline = true;
        } else if (IsComment(unit) && gap == 0) {
            newline = false; // 뒤 주석
        } else if (previous.kind == UnitKind::BlockComment && gap == 0 && previousLeads_ &&
                   !IsToken(unit, TokenKind::RightBrace)) {
            newline = false; // 줄 처음의 블록 주석 뒤(`/* c */ x();`)
        } else if (IsToken(unit, TokenKind::LeftBrace) || IsToken(unit, TokenKind::Comma) ||
                   IsToken(unit, TokenKind::Semicolon) ||
                   (IsToken(unit, TokenKind::Else) && hasSignificant_ &&
                    IsToken(significant_, TokenKind::RightBrace))) {
            newline = false;
        } else if ((hasSignificant_ && (IsToken(significant_, TokenKind::Semicolon) ||
                                        IsToken(significant_, TokenKind::LeftBrace) ||
                                        IsToken(significant_, TokenKind::RightBrace) ||
                                        IsToken(significant_, TokenKind::DocComment))) ||
                   IsToken(unit, TokenKind::DocComment) || IsToken(unit, TokenKind::RightBrace)) {
            newline = true;
        } else {
            newline = gap > 0;
        }

        blank = newline && gap >= 2 && !afterOpen_ && !IsToken(unit, TokenKind::RightBrace) &&
                !(hasSignificant_ && IsToken(significant_, TokenKind::DocComment));
    }

    // 다음 토큰 단위(at부터 — 주석은 건너뛴다)가 import인지다.
    bool NextIsImport(usize at) const noexcept {
        for (; at < units_.Size(); ++at) {
            if (!IsComment(units_[at])) {
                return IsToken(units_[at], TokenKind::Import);
            }
        }

        return false;
    }

    // 줄 처음 단위 at의 들여쓰기 공백 수다(8.2절 "들여쓰기").
    uint32 Indent(usize at) const noexcept {
        const Unit& unit = units_[at];
        usize depth = braces_.Size();
        if (IsToken(unit, TokenKind::RightBrace) && depth > 0) {
            --depth;
        }

        // 줄 처음의 닫는 괄호들(원문에서 같은 줄로 이어진 것)은 빼고 센다.
        usize closers = 0;
        for (usize next = at; next < units_.Size() && IsCloser(units_[next]); ++next) {
            if (next > at && Newlines(units_[next - 1].end, units_[next].begin) != 0) {
                break;
            }

            ++closers;
        }

        const usize open = brackets_.Size() > closers ? brackets_.Size() - closers : 0;
        uint32 levels = 0;
        for (usize index = 0; index < open; ++index) {
            levels += index == 0 || brackets_[index] != brackets_[index - 1] ? 1 : 0;
        }

        levels += continued_ && !IsToken(unit, TokenKind::LeftBrace) ? 1 : 0;
        return static_cast<uint32>(depth) * IndentWidth + levels * IndentWidth;
    }

    // 원문 [begin, end)를 붙인다: CRLF는 LF, trim이면 줄마다 끝 공백·탭을 지운다. 줄바꿈 수를 센다.
    Result<void> AppendText(uint32 begin, uint32 end, bool trim) noexcept {
        uint32 lineBegin = begin;
        while (lineBegin <= end) {
            uint32 lineEnd = lineBegin;
            while (lineEnd < end && text_.Data()[lineEnd] != '\n') {
                ++lineEnd;
            }

            uint32 contentEnd = lineEnd;
            if (lineEnd < end && contentEnd > lineBegin && text_.Data()[contentEnd - 1] == '\r') {
                --contentEnd; // CRLF의 CR
            }

            while (trim && contentEnd > lineBegin &&
                   (text_.Data()[contentEnd - 1] == ' ' || text_.Data()[contentEnd - 1] == '\t')) {
                --contentEnd;
            }

            if (!out_.TryAppend(StringView(text_.Data() + lineBegin, contentEnd - lineBegin))) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            if (lineEnd == end) {
                break;
            }

            if (!out_.TryAppend("\n")) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            ++line_;
            lineBegin = lineEnd + 1;
        }

        return Result<void>::Success();
    }

    Result<void> Place(usize at) noexcept {
        const Unit& unit = units_[at];
        bool newline = false;
        bool blank = false;
        if (at > 0) {
            Break(at, newline, blank);
        }

        // 최상위 항목 사이의 빈 줄.
        if (newline && topPending_) {
            blank = blank || !(topImport_ && NextIsImport(at));
            topPending_ = false;
        }

        if (newline) {
            if (AtBoundary() || IsToken(unit, TokenKind::RightBrace)) {
                continued_ = false;
            } else if (brackets_.IsEmpty()) {
                continued_ = true;
            }

            if (!out_.TryAppend(blank ? StringView("\n\n") : StringView("\n")) ||
                !out_.TryAppendRepeated(' ', Indent(at))) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            line_ += blank ? 2 : 1;
        } else if (at > 0 && SpaceBetween(units_[at - 1], unit) && !out_.TryAppend(" ")) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        previousLeads_ = at == 0 || newline;
        afterOpen_ = afterOpen_ && !newline; // `{` 뒤 첫 줄바꿈까지(같은 줄 뒤 주석 포함)
        const uint32 openLine = line_;
        if (auto appended = AppendText(unit.begin, unit.end,
                                       IsComment(unit) || IsToken(unit, TokenKind::DocComment));
            !appended) {
            return appended;
        }

        if (IsComment(unit)) {
            return Result<void>::Success();
        }

        // 괄호·중괄호 상태와 최상위 항목.
        if (braces_.IsEmpty() && topStart_) {
            currentImport_ = IsToken(unit, TokenKind::Import);
            topStart_ = false;
        }

        if (IsToken(unit, TokenKind::LeftBrace)) {
            afterOpen_ = true;
            Brace brace;
            brace.enumBody = (unit.flags & UnitFlags::EnumBrace) != 0;
            brace.brackets = static_cast<uint32>(brackets_.Size());
            if (auto pushed = braces_.TryPushBack(brace); !pushed) {
                return pushed;
            }
        } else if (IsToken(unit, TokenKind::RightBrace) && !braces_.IsEmpty()) {
            static_cast<void>(braces_.TryPopBack());
        } else if (IsToken(unit, TokenKind::LeftParen) || IsToken(unit, TokenKind::LeftBracket)) {
            if (auto pushed = brackets_.TryPushBack(openLine); !pushed) {
                return pushed;
            }
        } else if (IsCloser(unit) && !brackets_.IsEmpty()) {
            static_cast<void>(brackets_.TryPopBack());
        }

        if (braces_.IsEmpty() && brackets_.IsEmpty() &&
            (IsToken(unit, TokenKind::Semicolon) || IsToken(unit, TokenKind::RightBrace))) {
            topPending_ = true;
            topStart_ = true;
            topImport_ = currentImport_;
        }

        significant_ = unit;
        hasSignificant_ = true;
        return Result<void>::Success();
    }

    StringView text_;
    StringBuilderBase& out_;
    Array<Unit> units_;
    Array<Brace> braces_;
    Array<uint32> brackets_; // 열린 `(`·`[`마다 그것을 쓴 출력 줄
    uint32 line_ = 0;
    bool continued_ = false;     // 문장이 괄호 밖에서 줄을 바꿔 이어진다
    bool topPending_ = false;    // 최상위 항목이 끝났다(다음 새 줄 앞에 빈 줄)
    bool topStart_ = true;       // 다음 최상위 토큰이 항목의 시작이다
    bool topImport_ = false;     // 끝난 최상위 항목이 import다
    bool currentImport_ = false; // 지금 최상위 항목이 import다
    Unit significant_;           // 마지막 토큰 단위(주석 제외)
    bool hasSignificant_ = false;
    bool previousLeads_ = false; // 앞 단위가 줄 처음이다
    bool afterOpen_ = false;     // `{` 뒤 아직 줄을 바꾸지 않았다
};

// 트리 토큰에서 index 토큰의 끝이 end인 토큰을 찾는다(없으면 NoToken).
uint32 TokenEndingAt(Span<const Token> tokens, uint32 end) noexcept {
    usize low = 0;
    usize high = tokens.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (tokens[middle].span.begin < end) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low > 0 && tokens[low - 1].span.end == end ? static_cast<uint32>(low - 1) : NoToken;
}

// 트리에서 토큰 표지를 모은다(8.1절 "구문 정보"). closeBefore[i]는 i 토큰 앞에 되살릴 `>`가
// 있음이다(타입 인자를 닫는 `>=`가 `=`로 바뀐 자리).
void MarkTokens(const SyntaxTree& tree, Span<uint8> marks, Span<uint8> closeBefore) noexcept {
    const Span<const Token> tokens = tree.Tokens();
    auto markTypeArguments = [&](uint32 open) noexcept {
        if (open >= tokens.Size() || tokens[open].kind != TokenKind::Less) {
            return;
        }

        marks[open] |= UnitFlags::TypeOpen;
        uint32 depth = 1;
        for (uint32 at = open + 1; at < tokens.Size() && depth > 0; ++at) {
            switch (tokens[at].kind) {
            case TokenKind::Less:
                ++depth;
                break;
            case TokenKind::Greater:
                if (--depth == 0) {
                    marks[at] |= UnitFlags::TypeClose;
                }

                break;
            case TokenKind::Equal: // 타입 인자를 닫는 `>=`가 바뀐 `=`
                if (--depth == 0) {
                    closeBefore[at] = 1;
                }

                break;
            default:
                break;
            }
        }
    };
    for (uint32 node = 0; node < tree.NodeCount(); ++node) {
        const SyntaxNode& entry = tree.Node(node);
        switch (entry.kind) {
        case SyntaxKind::Unary:
            if (entry.token < marks.Size()) {
                marks[entry.token] |= UnitFlags::Unary;
            }

            break;
        case SyntaxKind::Type: {
            const uint32 path = tree.Child(node, 0);
            if (tree.Child(node, 1) != NoNode && path != NoNode && !tree.Children(path).IsEmpty()) {
                const uint32 last = tree.Children(path)[tree.Children(path).Size() - 1];
                if (last != NoNode) {
                    markTypeArguments(tree.Node(last).token + 1);
                }
            }

            break;
        }
        case SyntaxKind::Identifier:
            if (entry.childCount > 0 && tree.Child(node, 0) != NoNode) {
                markTypeArguments(entry.token + 1);
            }

            break;
        case SyntaxKind::Member:
            if (entry.childCount > 1 && tree.Child(node, 1) != NoNode) {
                markTypeArguments(entry.token + 1);
            }

            break;
        case SyntaxKind::Attribute: {
            const uint32 last = TokenEndingAt(tokens, entry.span.end);
            if (last != NoToken) {
                marks[last] |= UnitFlags::AttributeEnd;
            }

            break;
        }
        case SyntaxKind::Class: {
            const uint32 name = tree.Child(node, 2);
            if (name != NoNode) {
                const uint32 colon = tree.Node(name).token + 1;
                if (colon < tokens.Size() && tokens[colon].kind == TokenKind::Colon) {
                    marks[colon] |= UnitFlags::BaseColon;
                }
            }

            break;
        }
        case SyntaxKind::Enum: {
            const uint32 name = tree.Child(node, 2);
            if (name != NoNode) {
                const uint32 brace = tree.Node(name).token + 1;
                if (brace < tokens.Size() && tokens[brace].kind == TokenKind::LeftBrace) {
                    marks[brace] |= UnitFlags::EnumBrace;
                }
            }

            break;
        }
        default:
            break;
        }
    }
}

// 트리 토큰과 주석을 원문 순서의 단위로 모은다. 문자열 리터럴은 한 단위다(그 안의 주석은 버린다).
Result<void> CollectUnits(StringView text, const SyntaxTree& tree, Span<const SourceSpan> comments,
                          Span<const uint8> marks, Span<const uint8> closeBefore,
                          Array<Unit>& units) noexcept {
    const Span<const Token> tokens = tree.Tokens();
    usize comment = 0;
    auto pushComments = [&](uint32 before) noexcept -> Result<void> {
        for (; comment < comments.Size() && comments[comment].begin < before; ++comment) {
            Unit unit;
            unit.begin = comments[comment].begin;
            unit.end = comments[comment].end;
            unit.kind =
                text.Data()[unit.begin + 1] == '*' ? UnitKind::BlockComment : UnitKind::LineComment;
            if (auto pushed = units.TryPushBack(unit); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    };
    for (uint32 at = 0; at < tokens.Size(); ++at) {
        const Token& token = tokens[at];
        if (token.kind == TokenKind::EndOfFile) {
            break;
        }

        if (auto pushed = pushComments(token.span.begin); !pushed) {
            return pushed;
        }

        if (closeBefore[at] != 0) {
            Unit close;
            close.token = TokenKind::Greater;
            close.flags = UnitFlags::TypeClose;
            close.begin = token.span.begin - 1;
            close.end = token.span.begin;
            if (auto pushed = units.TryPushBack(close); !pushed) {
                return pushed;
            }
        }

        Unit unit;
        unit.token = token.kind;
        unit.flags = marks[at];
        unit.begin = token.span.begin;
        unit.end = token.span.end;
        if (token.kind == TokenKind::StringBegin) {
            // 닫는 `"`까지 한 단위(보간 안의 문자열은 진단이지만 구문은 받는다 — 깊이를 센다).
            uint32 depth = 0;
            uint32 last = at;
            for (; last < tokens.Size() && tokens[last].kind != TokenKind::EndOfFile; ++last) {
                depth += tokens[last].kind == TokenKind::StringBegin ? 1 : 0;
                depth -= tokens[last].kind == TokenKind::StringEnd ? 1 : 0;
                if (depth == 0) {
                    break;
                }
            }

            unit.kind = UnitKind::Text;
            unit.end = tokens[last].span.end;
            at = last;
            while (comment < comments.Size() && comments[comment].begin < unit.end) {
                ++comment;
            }
        }

        if (auto pushed = units.TryPushBack(unit); !pushed) {
            return pushed;
        }
    }

    return pushComments(NumericLimits<uint32>::Max());
}

} // namespace

Result<bool> FormatSource(Allocator& allocator, const SourceSet& sources, SourceId id,
                          DiagnosticList& diagnostics, StringBuilderBase& out) noexcept {
    const SourceFile& file = sources.Get(id);
    if (!file.IsUsable()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const usize before = diagnostics.Count();
    SyntaxTree tree(allocator);
    if (auto parsed = ParseFile(sources, id, diagnostics, tree); !parsed) {
        return Result<bool>::Failure(parsed.Error());
    }

    if (diagnostics.Count() != before) {
        return Result<bool>::Success(false);
    }

    // 주석 범위(어휘 진단은 위에서 없음을 봤다).
    Array<Token> scratch(allocator);
    Array<SourceSpan> comments(allocator);
    DiagnosticList ignored(allocator);
    if (auto lexed = TokenizeWithComments(sources, id, ignored, scratch, comments); !lexed) {
        return Result<bool>::Failure(lexed.Error());
    }

    const usize count = tree.Tokens().Size();
    Array<uint8> marks(allocator);
    Array<uint8> closeBefore(allocator);
    if (!marks.TryResize(count) || !closeBefore.TryResize(count)) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize at = 0; at < count; ++at) {
        marks[at] = 0;
        closeBefore[at] = 0;
    }

    MarkTokens(tree, marks.View(), closeBefore.View());
    Formatter formatter(allocator, file.Text(), out);
    if (auto collected = CollectUnits(file.Text(), tree, comments.View(), marks.View(),
                                      closeBefore.View(), formatter.Units());
        !collected) {
        return Result<bool>::Failure(collected.Error());
    }

    if (auto ran = formatter.Run(); !ran) {
        return Result<bool>::Failure(ran.Error());
    }

    return Result<bool>::Success(true);
}

} // namespace rocks::sparks
