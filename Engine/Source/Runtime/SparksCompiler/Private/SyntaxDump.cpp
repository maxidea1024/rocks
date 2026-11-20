#include <SparksCompiler/SyntaxTree.h>

#include <Core/Containers/Array.h>

// 구문 트리 덤프다(SD58, 형식은 SyntaxTree.h의 AppendSyntaxDump 설명). 깊은 트리도 stack을 쓰지
// 않게 명시 stack으로 걷는다.

namespace rocks::sparks {

namespace {

// 깊이 한 단계의 들여쓰기 칸 수다.
constexpr usize SyntaxDumpIndentSpaces = 2;

// 그대로 쓸 수 없는 제어 문자의 범위다(U+0000–U+001F와 U+007F).
constexpr uint8 FirstPrintableAscii = 0x20;
constexpr uint8 AsciiDelete = 0x7F;

// 4비트 값 하나의 16진 숫자 수다(`\u{XX}`는 두 자리).
constexpr uint32 HexDigitBits = 4;
constexpr uint8 HexDigitMask = 0x0F;

struct DumpEntry {
    uint32 node = NoNode;
    uint32 depth = 0;
};

// 덤프 줄 하나에 쓰는 flag 이름이다(SyntaxTree.h에 적은 순서).
struct FlagName {
    uint32 flag;
    StringView name;
};

constexpr FlagName DumpFlagNames[] = {
    {SyntaxFlags::Open, "open"},         {SyntaxFlags::Override, "override"},
    {SyntaxFlags::Async, "async"},       {SyntaxFlags::Let, "let"},
    {SyntaxFlags::Optional, "optional"}, {SyntaxFlags::NullConditional, "null-conditional"},
    {SyntaxFlags::Await, "await"},
};

// 토큰 글자를 따옴표 안에 escape해 붙인다. text는 올바른 UTF-8이다(사용 가능한 원문, S0101 없음).
Result<void> AppendQuotedText(StringBuilderBase& out, StringView text) noexcept {
    constexpr StringView HexDigits = "0123456789ABCDEF";
    out.Append("\"");
    usize runBegin = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const uint8 byte = static_cast<uint8>(text.Data()[index]);
        StringView escape;
        switch (byte) {
        case '\\':
            escape = "\\\\";
            break;
        case '"':
            escape = "\\\"";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }

        const bool control = byte < FirstPrintableAscii || byte == AsciiDelete;
        if (escape.IsEmpty() && !control) {
            continue;
        }

        // ASCII byte에서만 끊으므로 이어 붙이는 조각은 늘 올바른 UTF-8이다.
        out.Append(StringView(text.Data() + runBegin, index - runBegin));
        if (!escape.IsEmpty()) {
            out.Append(escape);
        } else {
            const char digits[] = {HexDigits.Data()[byte >> HexDigitBits],
                                   HexDigits.Data()[byte & HexDigitMask]};
            out.Append("\\u{").Append(StringView(digits, sizeof(digits))).Append("}");
        }

        runBegin = index + 1;
    }

    out.Append(StringView(text.Data() + runBegin, text.ByteCount() - runBegin));
    out.Append("\"");
    return out.Status();
}

Result<void> AppendNodeLine(StringBuilderBase& out, const SourceFile& file, const SyntaxTree& tree,
                            DumpEntry entry) noexcept {
    if (auto indented = out.TryAppendRepeated(' ', entry.depth * SyntaxDumpIndentSpaces);
        !indented) {
        return indented;
    }

    if (entry.node == NoNode) {
        out.Append("-\n");
        return out.Status();
    }

    const SyntaxNode& node = tree.Node(entry.node);
    out.Append(SyntaxKindName(node.kind));
    for (const FlagName& flag : DumpFlagNames) {
        if ((node.flags & flag.flag) != 0) {
            out.Append(" ").Append(flag.name);
        }
    }

    if (node.token != NoToken) {
        const SourceSpan span = tree.Tokens()[node.token].span;
        out.Append(" ");
        if (auto quoted = AppendQuotedText(
                out, StringView(file.Text().Data() + span.begin, span.end - span.begin));
            !quoted) {
            return quoted;
        }
    }

    if (node.kind == SyntaxKind::DocComment) {
        out.AppendFormat(" lines={}", node.extra);
    }

    const SourceLocation begin = file.Locate(node.span.begin);
    const SourceLocation end = file.Locate(node.span.end);
    out.AppendFormat(" @{}:{}-{}:{}\n", begin.line, begin.column, end.line, end.column);
    return out.Status();
}

} // namespace

Result<void> AppendSyntaxDump(StringBuilderBase& out, const SourceSet& sources,
                              const SyntaxTree& tree) noexcept {
    if (tree.Root() == NoNode) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SourceFile& file = sources.Get(tree.Source());
    Array<DumpEntry> pending(out.GetAllocator());
    if (auto pushed = pending.TryPushBack(DumpEntry{tree.Root(), 0}); !pushed) {
        return pushed;
    }

    while (!pending.IsEmpty()) {
        auto popped = pending.TryPopBack();
        if (!popped) {
            return Result<void>::Failure(popped.Error());
        }

        const DumpEntry entry = popped.Value();
        if (auto written = AppendNodeLine(out, file, tree, entry); !written) {
            return written;
        }

        if (entry.node == NoNode) {
            continue;
        }

        // 첫 자식이 먼저 나오도록 거꾸로 쌓는다.
        const Span<const uint32> children = tree.Children(entry.node);
        for (usize index = children.Size(); index > 0; --index) {
            if (auto pushed = pending.TryPushBack(DumpEntry{children[index - 1], entry.depth + 1});
                !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
