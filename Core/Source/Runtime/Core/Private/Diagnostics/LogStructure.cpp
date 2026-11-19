#include "LogStructureEncoding.h"

#include <Core/Diagnostics/LogCapture.h>
#include <Platform/Memory.h>

// 펼친 값("{@Name}", LG16)의 인코딩 쓰기(LogStructureWriter)와 읽기·렌더다. 형식은
// LogStructureEncoding.h에 있다.
namespace rocks {

using detail::log_structure::Record;

namespace log_structure = detail::log_structure;

namespace {

constexpr uint8 RecordByte(Record kind) noexcept {
    return static_cast<uint8>(kind);
}

// 정적 문자열을 담을 수 있는 길이로 자른다(길이 칸이 16-bit다).
StringView ClampStatic(StringView text) noexcept {
    return text.ByteCount() <= MaxLogStaticNameBytes
               ? text
               : text.LeftBytesAtCodePoint(MaxLogStaticNameBytes);
}

} // namespace

// ---- LogStructureWriter ----
//
// 공간 규칙: 기록을 쓰기 전에 "그 기록 + 열린 객체·배열의 End"가 들어가는지 본다(Available). 그래서
// 공간이 바닥나도 이미 연 객체·배열은 항상 닫을 수 있고, 인코딩은 늘 올바른 모양이다(Validate).
// 바닥나면(Exhaust) 열린 객체·배열에 잘림 표시를 남기고 그 뒤의 자식은 모두 버린다.

LogStructureWriter::LogStructureWriter(Span<uint8> buffer) noexcept : buffer_(buffer) {
}

usize LogStructureWriter::Available() const noexcept {
    const usize reserved = used_ + depth_ * log_structure::EndBytes;
    return buffer_.Size() > reserved ? buffer_.Size() - reserved : 0;
}

bool LogStructureWriter::InArray() const noexcept {
    return depth_ != 0 && frames_[depth_ - 1].array;
}

void LogStructureWriter::MarkFrameTruncated(const Frame& frame) noexcept {
    buffer_[frame.offset] |= log_structure::Truncated;
    truncated_ = true;
}

void LogStructureWriter::MarkSpaceShort() noexcept {
    // 공간 때문에 잃은 내용은 그것을 담은 모든 객체·배열의 내용이다(텍스트의 "..."와 JSON의
    // "$truncated"가 조상까지 같게 보인다).
    spaceShort_ = true;
    truncated_ = true;
    for (usize index = 0; index < depth_; ++index) {
        MarkFrameTruncated(frames_[index]);
    }
}

void LogStructureWriter::Exhaust() noexcept {
    exhausted_ = true;
    MarkSpaceShort();
}

bool LogStructureWriter::BeginChild() noexcept {
    if (finished_ || skip_ != 0) {
        return false;
    }

    // 맨 위 값(객체 하나)은 BeginContainer가 따로 판정한다. 맨 위의 그 밖의 쓰기는 버린다.
    if (depth_ == 0) {
        return false;
    }

    Frame& parent = frames_[depth_ - 1];
    ++parent.children;
    const usize limit = parent.array ? MaxLogStructureElements : MaxLogStructureFields;
    if (exhausted_ || parent.children > limit) {
        MarkFrameTruncated(parent);
        return false;
    }

    return true;
}

bool LogStructureWriter::PutHeader(uint8 kind, LogStaticName name, usize payloadBytes) noexcept {
    // 객체의 필드만 이름을 담는다(맨 위 객체와 배열 원소는 이름이 없다).
    const bool named = depth_ != 0 && !InArray();
    const usize header = log_structure::HeadBytes + (named ? log_structure::StaticTextBytes : 0);
    if (header + payloadBytes > Available()) {
        Exhaust();
        return false;
    }

    buffer_[used_++] = static_cast<uint8>(kind | (named ? log_structure::HasName : 0));
    if (named) {
        PutStatic(name.View());
    }

    return true;
}

void LogStructureWriter::PutBytes(const void* data, usize bytes) noexcept {
    if (bytes != 0) {
        platform::MemoryCopy(buffer_.Data() + used_, data, bytes);
        used_ += bytes;
    }
}

void LogStructureWriter::PutStatic(StringView text) noexcept {
    const StringView kept = ClampStatic(text);
    const char* pointer = kept.Data();
    const auto bytes = static_cast<uint16>(kept.ByteCount());
    PutBytes(&pointer, sizeof(pointer));
    PutBytes(&bytes, sizeof(bytes));
}

void LogStructureWriter::WriteNull(LogStaticName name) noexcept {
    if (BeginChild()) {
        (void)PutHeader(RecordByte(Record::Null), name, 0);
    }
}

void LogStructureWriter::WriteBool(LogStaticName name, bool value) noexcept {
    if (BeginChild() && PutHeader(RecordByte(Record::Bool), name, log_structure::BoolBytes)) {
        const uint8 bit = value ? 1 : 0;
        PutBytes(&bit, log_structure::BoolBytes);
    }
}

void LogStructureWriter::WriteSigned(LogStaticName name, int64 value) noexcept {
    if (BeginChild() && PutHeader(RecordByte(Record::Signed), name, log_structure::ScalarBytes)) {
        PutBytes(&value, sizeof(value));
    }
}

void LogStructureWriter::WriteUnsigned(LogStaticName name, uint64 value) noexcept {
    if (BeginChild() && PutHeader(RecordByte(Record::Unsigned), name, log_structure::ScalarBytes)) {
        PutBytes(&value, sizeof(value));
    }
}

void LogStructureWriter::WriteFloat(LogStaticName name, double value) noexcept {
    if (BeginChild() && PutHeader(RecordByte(Record::Float), name, log_structure::ScalarBytes)) {
        const uint64 bits = platform::BitCast<uint64>(value);
        PutBytes(&bits, sizeof(bits));
    }
}

void LogStructureWriter::WriteString(LogStaticName name, StringView value) noexcept {
    const usize offset = used_;
    if (!BeginChild() || !PutHeader(RecordByte(Record::String), name, log_structure::LengthBytes)) {
        return;
    }

    // 머리와 길이 칸 뒤에 남은 만큼 담는다(열린 객체·배열의 End 자리는 Available이 이미 뺐다).
    usize room = Available() - log_structure::LengthBytes;
    if (room > NumericLimits<uint32>::Max()) {
        room = NumericLimits<uint32>::Max();
    }

    const StringView kept = value.LeftBytesAtCodePoint(room);
    if (kept.ByteCount() < value.ByteCount()) {
        buffer_[offset] |= log_structure::Truncated;
        MarkSpaceShort();
    }

    const auto bytes = static_cast<uint32>(kept.ByteCount());
    PutBytes(&bytes, sizeof(bytes));
    PutBytes(kept.Data(), kept.ByteCount());
}

void LogStructureWriter::WriteCharacterArray(LogStaticName name, const char* value,
                                             usize capacity) noexcept {
    // char 배열은 첫 NUL까지, NUL이 없으면 배열 끝까지다(MakeFormatArg와 같은 규칙).
    usize length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }

    WriteString(name, StringView(value, length));
}

void LogStructureWriter::WriteSymbol(LogStaticName name, LogStaticName symbol) noexcept {
    if (BeginChild() &&
        PutHeader(RecordByte(Record::Symbol), name, log_structure::StaticTextBytes)) {
        PutStatic(symbol.View());
    }
}

void LogStructureWriter::WriteUnsupported(LogStaticName name) noexcept {
    if (BeginChild()) {
        (void)PutHeader(RecordByte(Record::Unsupported), name, 0);
    }
}

void LogStructureWriter::WriteFormattedErased(LogStaticName name, const void* object,
                                              detail::FormatArg::CustomFunction format) noexcept {
    const usize offset = used_;
    if (!BeginChild() || !PutHeader(RecordByte(Record::String), name,
                                    log_structure::LengthBytes + MinimumLogRenderBytes)) {
        return;
    }

    // 길이 칸을 비워 두고 남은 영역에 바로 렌더한 뒤 길이를 채운다(Formatter는 한 번만 부른다).
    const usize lengthAt = used_;
    used_ += log_structure::LengthBytes;
    FixedStringBuilder text(
        Span<char>(reinterpret_cast<char*>(buffer_.Data() + used_), Available()));
    if (!format(text, object, StringView())) {
        // 그 값만 "<format error>"다: 머리만 남는 FormatError 기록으로 바꾼다.
        buffer_[offset] = static_cast<uint8>((buffer_[offset] & ~log_structure::KindMask) |
                                             RecordByte(Record::FormatError));
        used_ = lengthAt;
        return;
    }

    const auto bytes = static_cast<uint32>(text.ByteCount());
    platform::MemoryCopy(buffer_.Data() + lengthAt, &bytes, sizeof(bytes));
    used_ += text.ByteCount();
    if (text.Truncated()) {
        buffer_[offset] |= log_structure::Truncated;
        MarkSpaceShort();
    }
}

bool LogStructureWriter::BeginContainer(uint8 kind, LogStaticName name, LogStaticName tag,
                                        usize totalCount) noexcept {
    const bool array = kind == RecordByte(Record::Array);
    // 맨 위에는 객체 하나만 둔다.
    const bool root = depth_ == 0;
    const bool accepted =
        root ? !array && used_ == 0 && !exhausted_ && !finished_ && skip_ == 0 : BeginChild();
    if (!accepted) {
        ++skip_;
        return false;
    }

    const usize offset = used_;
    const usize payload = array ? log_structure::ScalarBytes : log_structure::StaticTextBytes;
    // 자기 End 자리도 함께 확인한다.
    if (!PutHeader(kind, name, payload + log_structure::EndBytes)) {
        ++skip_;
        return false;
    }

    if (array) {
        const auto count = static_cast<uint64>(totalCount);
        PutBytes(&count, sizeof(count));
    } else {
        PutStatic(tag.View());
    }

    // 깊이 상한: 머리와 End만 남기고(잘림 표시) 내용은 버린다.
    if (depth_ == MaxLogStructureDepth) {
        buffer_[offset] |= log_structure::Truncated;
        buffer_[used_++] = RecordByte(Record::End);
        truncated_ = true;
        ++skip_;
        return false;
    }

    frames_[depth_] = Frame{offset, 0, array};
    ++depth_;
    if (array && totalCount > MaxLogStructureElements) {
        MarkFrameTruncated(frames_[depth_ - 1]);
    }

    return true;
}

void LogStructureWriter::EndContainer() noexcept {
    if (skip_ != 0) {
        --skip_;
        return;
    }

    if (depth_ == 0) {
        return;
    }

    // End 자리는 Begin 때부터 남겨 두었다.
    buffer_[used_++] = RecordByte(Record::End);
    --depth_;
    if (depth_ == 0) {
        finished_ = true;
    }
}

bool LogStructureWriter::BeginObject(LogStaticName name, LogStaticName tag) noexcept {
    return BeginContainer(RecordByte(Record::Object), name, tag, 0);
}

void LogStructureWriter::EndObject() noexcept {
    EndContainer();
}

bool LogStructureWriter::BeginArray(LogStaticName name, usize totalCount) noexcept {
    return BeginContainer(RecordByte(Record::Array), name, {}, totalCount);
}

void LogStructureWriter::EndArray() noexcept {
    EndContainer();
}

void LogStructureWriter::Finish() noexcept {
    // 짝이 맞지 않게 열린 객체·배열을 닫는다(End 자리는 남겨 두었다).
    while (depth_ != 0) {
        buffer_[used_++] = RecordByte(Record::End);
        --depth_;
    }

    skip_ = 0;
    finished_ = true;
}

// ---- 읽기 ----

namespace detail::log_structure {

bool Cursor::Read(void* out, usize bytes) noexcept {
    if (bytes > stream_.Size() - position_) {
        failed_ = true;
        return false;
    }

    platform::MemoryCopy(out, stream_.Data() + position_, bytes);
    position_ += bytes;
    return true;
}

bool Cursor::ReadStatic(StringView& text) noexcept {
    const char* pointer = nullptr;
    uint16 bytes = 0;
    if (!Read(&pointer, sizeof(pointer)) || !Read(&bytes, sizeof(bytes))) {
        return false;
    }

    if (pointer == nullptr && bytes != 0) {
        failed_ = true;
        return false;
    }

    text = StringView(pointer, bytes);
    return true;
}

bool Cursor::Next(Node& node) noexcept {
    if (failed_ || position_ >= stream_.Size()) {
        return false;
    }

    const uint8 head = stream_[position_++];
    const uint8 kind = head & KindMask;
    if (kind < RecordByte(Record::Null) || kind > RecordByte(Record::End) ||
        (head & ~(KindMask | HasName | Truncated)) != 0) {
        failed_ = true;
        return false;
    }

    node = Node{};
    node.kind = static_cast<Record>(kind);
    node.truncated = (head & Truncated) != 0;
    node.hasName = (head & HasName) != 0;
    if (node.hasName && !ReadStatic(node.name)) {
        return false;
    }

    switch (node.kind) {
    case Record::Bool: {
        uint8 bit = 0;
        if (!Read(&bit, BoolBytes)) {
            return false;
        }

        if (bit > 1) {
            failed_ = true;
            return false;
        }

        node.bits = bit;
        return true;
    }
    case Record::Signed:
    case Record::Unsigned:
    case Record::Float:
    case Record::Array:
        return Read(&node.bits, sizeof(node.bits));
    case Record::String: {
        uint32 bytes = 0;
        if (!Read(&bytes, sizeof(bytes))) {
            return false;
        }

        if (bytes > stream_.Size() - position_) {
            failed_ = true;
            return false;
        }

        node.text = StringView(reinterpret_cast<const char*>(stream_.Data() + position_), bytes);
        position_ += bytes;
        return true;
    }
    case Record::Symbol:
    case Record::Object:
        return ReadStatic(node.text);
    case Record::Null:
    case Record::Unsupported:
    case Record::FormatError:
    case Record::End:
        return true;
    }

    return true;
}

bool Validate(Span<const uint8> stream) noexcept {
    if (stream.Size() == 0) {
        return true;
    }

    Cursor cursor(stream);
    Node node;
    bool arrays[MaxLogStructureDepth]{};
    usize depth = 0;
    bool rootClosed = false;
    bool sawRoot = false;
    while (cursor.Next(node)) {
        if (rootClosed) {
            return false; // 맨 위 객체 뒤에는 아무것도 없어야 한다
        }

        if (node.kind == Record::End) {
            if (depth == 0 || node.hasName) {
                return false;
            }

            --depth;
            rootClosed = depth == 0;
            continue;
        }

        // 맨 위는 이름 없는 객체 하나다. 그 아래는 객체 필드만 이름을 가진다.
        if (depth == 0) {
            if (sawRoot || node.kind != Record::Object || node.hasName) {
                return false;
            }

            sawRoot = true;
        } else if (node.hasName == arrays[depth - 1]) {
            return false;
        }

        if (node.kind == Record::Object || node.kind == Record::Array) {
            // 깊이 상한에서 잘린 객체·배열은 잘림 표시와 바로 뒤 End만 가진다(내용 없음).
            if (depth == MaxLogStructureDepth) {
                Node end;
                if (!node.truncated || !cursor.Next(end) || end.kind != Record::End ||
                    end.hasName) {
                    return false;
                }

                continue;
            }

            arrays[depth] = node.kind == Record::Array;
            ++depth;
        }
    }

    return !cursor.Failed() && rootClosed;
}

namespace {

// 문자열 하나를 따옴표와 escape로 붙인다(JSON 문자열과 같은 escape). 잘못된 UTF-8이면 false다.
bool AppendQuoted(FixedStringBuilder& out, StringView text) noexcept {
    if (!text.IsValidUtf8()) {
        (void)out.TryAppend("<format error>");
        return false;
    }

    // escape할 byte(따옴표·역슬래시·제어 문자)가 없는 구간은 통째로 붙인다.
    constexpr uint8 FirstPrintable = 0x20;
    constexpr uint8 Delete = 0x7F;
    (void)out.TryAppend("\"");
    const char* data = text.Data();
    usize start = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const auto byte = static_cast<uint8>(data[index]);
        if (byte != '"' && byte != '\\' && byte >= FirstPrintable && byte != Delete) {
            continue;
        }

        (void)out.TryAppend(StringView(data + start, index - start));
        start = index + 1;
        switch (byte) {
        case '"':
            (void)out.TryAppend("\\\"");
            break;
        case '\\':
            (void)out.TryAppend("\\\\");
            break;
        case '\n':
            (void)out.TryAppend("\\n");
            break;
        case '\r':
            (void)out.TryAppend("\\r");
            break;
        case '\t':
            (void)out.TryAppend("\\t");
            break;
        default:
            (void)out.TryAppendFormat("\\u{:04x}", static_cast<uint32>(byte));
            break;
        }
    }

    (void)out.TryAppend(StringView(data + start, text.ByteCount() - start));
    (void)out.TryAppend("\"");
    return true;
}

// 정적 문자열(이름·기호·태그)을 붙인다. 잘못된 UTF-8이면 "<format error>"이고 false다.
bool AppendStatic(FixedStringBuilder& out, StringView text) noexcept {
    if (!text.IsValidUtf8()) {
        (void)out.TryAppend("<format error>");
        return false;
    }

    (void)out.TryAppend(text);
    return true;
}

void AppendNumber(FixedStringBuilder& out, const Node& node) noexcept {
    FormatArg arg;
    if (node.kind == Record::Signed) {
        arg.kind = FormatArgKind::Signed;
        arg.signedValue = static_cast<int64>(node.bits);
    } else if (node.kind == Record::Unsigned) {
        arg.kind = FormatArgKind::Unsigned;
        arg.unsignedValue = node.bits;
    } else {
        arg.kind = FormatArgKind::Double;
        arg.real = platform::BitCast<double>(node.bits);
    }

    (void)detail::AppendFormatArgument(out, arg, StringView(), false);
}

// 렌더 중인 객체·배열 하나다.
struct TextFrame {
    bool array = false;
    bool truncated = false;
    uint64 count = 0;
    usize children = 0;
};

void CloseText(FixedStringBuilder& out, const TextFrame& frame) noexcept {
    if (frame.array) {
        if (frame.truncated) {
            (void)out.TryAppendFormat(frame.children != 0 ? ", ... {} items" : "... {} items",
                                      frame.count);
        }

        (void)out.TryAppend("]");
        return;
    }

    if (frame.truncated) {
        (void)out.TryAppend(frame.children != 0 ? ", ..." : " ...");
    }

    (void)out.TryAppend(frame.children != 0 || frame.truncated ? " }" : "}");
}

} // namespace

bool AppendText(FixedStringBuilder& out, Span<const uint8> stream) noexcept {
    if (stream.Size() == 0) {
        (void)out.TryAppend("{ ... }");
        return false;
    }

    // 깊이 상한에서 잘린 객체·배열이 한 단계 더 열린다(바로 닫힌다).
    TextFrame frames[MaxLogStructureDepth + 1];
    usize depth = 0;
    bool failed = false;
    Cursor cursor(stream);
    Node node;
    while (cursor.Next(node)) {
        if (node.kind == Record::End) {
            if (depth != 0) {
                CloseText(out, frames[--depth]);
            }

            continue;
        }

        // 형제 구분과 필드 이름: 객체는 "{ A: 1, B: 2 }", 배열은 "[1, 2]"다.
        if (depth != 0) {
            TextFrame& parent = frames[depth - 1];
            if (parent.children != 0) {
                (void)out.TryAppend(", ");
            } else if (!parent.array) {
                (void)out.TryAppend(" ");
            }

            ++parent.children;
            if (!parent.array) {
                failed = !AppendStatic(out, node.name) || failed;
                (void)out.TryAppend(": ");
            }
        }

        switch (node.kind) {
        case Record::Object:
            if (!node.text.IsEmpty()) {
                failed = !AppendStatic(out, node.text) || failed;
                (void)out.TryAppend(" ");
            }

            (void)out.TryAppend("{");
            frames[depth++] = TextFrame{false, node.truncated, 0, 0};
            break;
        case Record::Array:
            (void)out.TryAppend("[");
            frames[depth++] = TextFrame{true, node.truncated, node.bits, 0};
            break;
        case Record::Null:
            (void)out.TryAppend("null");
            break;
        case Record::Bool:
            (void)out.TryAppend(node.bits != 0 ? "true" : "false");
            break;
        case Record::Signed:
        case Record::Unsigned:
        case Record::Float:
            AppendNumber(out, node);
            break;
        case Record::String:
            failed = !AppendQuoted(out, node.text) || failed;
            if (node.truncated) {
                (void)out.TryAppend("...");
            }

            break;
        case Record::Symbol:
            failed = !AppendStatic(out, node.text) || failed;
            break;
        case Record::Unsupported:
            (void)out.TryAppend("<unsupported>");
            break;
        case Record::FormatError:
            (void)out.TryAppend("<format error>");
            failed = true;
            break;
        case Record::End:
            break;
        }
    }

    return failed;
}

namespace {

constexpr uint64 DoubleExponentMask = 0x7FF0000000000000ull;
constexpr uint64 DoubleMantissaMask = 0x000FFFFFFFFFFFFFull;
constexpr uint64 DoubleSignBit = 0x8000000000000000ull;

// 숫자 노드의 기본 서식 텍스트를 texts에 쓰고 view를 돌려준다.
StringView NumberText(FixedStringBuilder& texts, const Node& node) noexcept {
    const usize start = texts.ByteCount();
    const bool truncated = texts.Truncated();
    AppendNumber(texts, node);
    if (texts.Truncated() && !truncated) {
        texts.RewindTo(start, truncated);
        return {};
    }

    const usize bytes = texts.ByteCount() - start;
    return bytes == 0 ? StringView() : StringView(texts.View().Data() + start, bytes);
}

// 기록 하나로 노드를 채운다(Object·Array의 자식 범위는 End에서 채운다).
void FillNode(const Node& node, LogProperty& property, NodeFill& fill) noexcept {
    property.kind = LogPropertyKind::String;
    property.signedValue = 0;
    switch (node.kind) {
    case Record::Object:
        property.kind = LogPropertyKind::Structure;
        property.typeTag = node.text.IsValidUtf8() ? node.text : StringView();
        property.truncated = node.truncated;
        return;
    case Record::Array:
        property.kind = LogPropertyKind::Array;
        property.truncated = node.truncated;
        property.unsignedValue = node.bits;
        return;
    case Record::Null:
        property.kind = LogPropertyKind::Null;
        property.text = "null";
        return;
    case Record::Bool:
        property.kind = LogPropertyKind::Bool;
        property.boolValue = node.bits != 0;
        property.text = node.bits != 0 ? StringView("true") : StringView("false");
        return;
    case Record::Signed:
        property.kind = LogPropertyKind::Signed;
        property.signedValue = static_cast<int64>(node.bits);
        property.text = NumberText(*fill.texts, node);
        return;
    case Record::Unsigned:
        property.kind = LogPropertyKind::Unsigned;
        property.unsignedValue = node.bits;
        property.text = NumberText(*fill.texts, node);
        return;
    case Record::Float:
        // NaN·무한대는 JSON 숫자가 아니므로 텍스트만 둔다(맨 위 속성과 같은 규칙).
        if ((node.bits & DoubleExponentMask) == DoubleExponentMask) {
            property.text = (node.bits & DoubleMantissaMask) != 0 ? StringView("nan")
                            : (node.bits & DoubleSignBit) != 0    ? StringView("-inf")
                                                                  : StringView("inf");
            return;
        }

        property.kind = LogPropertyKind::Float;
        property.floatValue = platform::BitCast<double>(node.bits);
        property.text = NumberText(*fill.texts, node);
        return;
    case Record::String:
        if (!node.text.IsValidUtf8()) {
            property.text = "<format error>";
            fill.formatError = true;
            return;
        }

        property.text = node.text;
        property.truncated = node.truncated;
        return;
    case Record::Symbol:
        if (!node.text.IsValidUtf8()) {
            property.text = "<format error>";
            fill.formatError = true;
            return;
        }

        property.text = node.text;
        return;
    case Record::Unsupported:
        property.text = "<unsupported>";
        return;
    case Record::FormatError:
        property.text = "<format error>";
        fill.formatError = true;
        return;
    case Record::End:
        return;
    }
}

// 노드 칸이 모자라 버린 객체·배열의 표지다(End에서 채울 노드가 없다).
constexpr usize DroppedNode = MaxSize;
// 맨 위 속성 자신의 표지다(그 직접 자식의 parent는 NoLogPropertyParent다).
constexpr usize TopNode = MaxSize - 1;

} // namespace

void FillNodes(Span<const uint8> stream, LogProperty& top, NodeFill& fill) noexcept {
    top.kind = LogPropertyKind::Structure;
    top.typeTag = {};
    top.truncated = stream.Size() == 0;
    top.parent = NoLogPropertyParent;
    top.subtreeBegin = static_cast<uint32>(fill.used);
    top.subtreeCount = 0;
    top.signedValue = 0;

    // 깊이 상한에서 잘린 객체·배열이 한 단계 더 열린다(바로 닫힌다).
    usize open[MaxLogStructureDepth + 1];
    usize depth = 0;
    Cursor cursor(stream);
    Node node;
    while (cursor.Next(node)) {
        if (node.kind == Record::End) {
            const usize index = open[--depth];
            if (index == TopNode) {
                top.subtreeCount = static_cast<uint32>(fill.used - top.subtreeBegin);
            } else if (index != DroppedNode) {
                fill.nodes[index].subtreeCount = static_cast<uint32>(fill.used - index - 1);
            }

            continue;
        }

        const bool container = node.kind == Record::Object || node.kind == Record::Array;
        if (depth == 0) {
            // 맨 위 객체는 top 자신이다.
            top.typeTag = node.text.IsValidUtf8() ? node.text : StringView();
            top.truncated = node.truncated;
            open[depth++] = TopNode;
            continue;
        }

        if (fill.used == fill.nodes.Size()) {
            // 노드 칸이 없으면(빈 span) 노드를 채우지 않겠다는 요청이라
            // 잘림이 아니다. 칸이 모자라 버린 노드는 그것을 담은 모든 열린
            // 객체·배열의 내용 손실이다(JSON "$truncated"가 조상까지 보인다).
            if (fill.nodes.Size() != 0) {
                fill.overflow = true;
                for (usize level = 0; level < depth; ++level) {
                    if (open[level] == TopNode) {
                        top.truncated = true;
                    } else if (open[level] != DroppedNode) {
                        fill.nodes[open[level]].truncated = true;
                    }
                }
            }

            if (container) {
                open[depth++] = DroppedNode;
            }

            continue;
        }

        const usize parent = open[depth - 1];
        const usize index = fill.used++;
        LogProperty& property = fill.nodes[index];
        property = LogProperty{};
        property.name = node.hasName && node.name.IsValidUtf8() ? node.name : StringView();
        property.parent = parent == TopNode || parent == DroppedNode ? NoLogPropertyParent
                                                                     : static_cast<uint32>(parent);
        FillNode(node, property, fill);
        if (container) {
            property.subtreeBegin = static_cast<uint32>(index + 1);
            open[depth++] = index;
        }
    }
}

} // namespace detail::log_structure

} // namespace rocks
