#include "../Text/DecimalConversion.h"

#include <Core/Diagnostics/LogCapture.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

using detail::FormatArg;
using detail::FormatArgKind;
using detail::LogCaptureHeader;
using detail::LogCaptureValue;

// 캡처 머리의 표지('RLOG')다. 손상되었거나 캡처가 아닌 byte를 렌더하지 않게 한다.
constexpr uint32 CaptureMagic = 0x474F4C52u;

// LogCaptureHeader::flags
constexpr uint8 CaptureRuntimeTemplate = 1u << 0;
constexpr uint8 CaptureValueTruncated = 1u << 1;

// LogCaptureValue::flags
constexpr uint8 ValueInBlock = 1u << 0;
constexpr uint8 ValueTruncated = 1u << 1;
constexpr uint8 ValueFormatError = 1u << 2;

constexpr StringView FormatErrorText = "<format error>";

static_assert(MaxLogArguments <= NumericLimits<uint16>::Max(), "value count is stored as uint16");

// 위치 자리의 속성 이름이다(Arg0…). MaxLogArguments개를 미리 둔다.
constexpr StringView PositionalPropertyNames[] = {
    "Arg0", "Arg1", "Arg2",  "Arg3",  "Arg4",  "Arg5",  "Arg6",  "Arg7",
    "Arg8", "Arg9", "Arg10", "Arg11", "Arg12", "Arg13", "Arg14", "Arg15"};

static_assert(sizeof(PositionalPropertyNames) / sizeof(PositionalPropertyNames[0]) ==
                  MaxLogArguments,
              "every argument position needs a property name");

// 캡처 한 건을 쓰는 동안의 상태다. 머리·값 칸 뒤의 남은 영역에 가변 데이터를 앞에서부터 쌓는다.
class CaptureWriter {
public:
    CaptureWriter(const LogCaptureTarget& target, usize cursor) noexcept
        : target_(target), cursor_(cursor) {
    }

    usize Cursor() const noexcept {
        return cursor_;
    }

    bool Truncated() const noexcept {
        return truncated_;
    }

    // text를 남은 영역에 담는다. 넘치면 블록을 빌리고, 블록도 없으면 UTF-8 경계에서 잘라 담는다.
    void StoreText(StringView text, LogCaptureValue& value) noexcept {
        if (text.ByteCount() <= Remaining()) {
            StoreInline(text, value);
            return;
        }

        LogBlock block;
        if (target_.blocks != nullptr && target_.blocks->TryBorrow(block)) {
            const StringView kept = text.LeftBytesAtCodePoint(block.bytes.Size());
            platform::MemoryCopy(block.bytes.Data(), kept.Data(), kept.ByteCount());
            value.flags |= ValueInBlock;
            value.payload = block.index;
            value.bytes = static_cast<uint32>(kept.ByteCount());
            MarkTruncated(value, kept.ByteCount() < text.ByteCount());
            return;
        }

        const StringView kept = text.LeftBytesAtCodePoint(Remaining());
        StoreInline(kept, value);
        MarkTruncated(value, true);
    }

    // 사용자 Formatter 결과를 남은 영역에 렌더한다. 넘치면 블록을 빌려 다시 렌더한다.
    void StoreCustom(const FormatArg& arg, StringView spec, LogCaptureValue& value) noexcept {
        value.kind = static_cast<uint8>(FormatArgKind::Custom);
        bool inlineTruncated = true;
        if (Remaining() >= 2) {
            FixedStringBuilder text(Span<char>(InlineData(), Remaining()));
            if (!arg.custom.format(text, arg.custom.object, spec)) {
                value.flags |= ValueFormatError;
                return;
            }

            value.payload = cursor_;
            value.bytes = static_cast<uint32>(text.ByteCount());
            inlineTruncated = text.Truncated();
            if (!inlineTruncated) {
                cursor_ += text.ByteCount();
                return;
            }
        }

        // 남은 영역에 다 들어가지 않았다. 블록이 있으면 그곳에 다시 렌더한다.
        LogBlock block;
        if (target_.blocks != nullptr && target_.blocks->TryBorrow(block)) {
            FixedStringBuilder text(
                Span<char>(reinterpret_cast<char*>(block.bytes.Data()), block.bytes.Size()));
            if (!arg.custom.format(text, arg.custom.object, spec)) {
                target_.blocks->Release(block.index);
                value.bytes = 0;
                value.flags |= ValueFormatError;
                return;
            }

            value.flags |= ValueInBlock;
            value.payload = block.index;
            value.bytes = static_cast<uint32>(text.ByteCount());
            MarkTruncated(value, text.Truncated());
            return;
        }

        // 블록이 없으면 남은 영역에 잘린 결과를 둔다(남은 영역이 없었으면 빈 값).
        if (Remaining() < 2) {
            value.payload = cursor_;
            value.bytes = 0;
        }

        cursor_ += value.bytes;
        MarkTruncated(value, true);
    }

private:
    usize Remaining() const noexcept {
        return target_.buffer.Size() - cursor_;
    }

    char* InlineData() noexcept {
        return reinterpret_cast<char*>(target_.buffer.Data() + cursor_);
    }

    void StoreInline(StringView text, LogCaptureValue& value) noexcept {
        platform::MemoryCopy(target_.buffer.Data() + cursor_, text.Data(), text.ByteCount());
        value.payload = cursor_;
        value.bytes = static_cast<uint32>(text.ByteCount());
        cursor_ += text.ByteCount();
    }

    void MarkTruncated(LogCaptureValue& value, bool truncated) noexcept {
        if (truncated) {
            value.flags |= ValueTruncated;
            truncated_ = true;
        }
    }

    const LogCaptureTarget& target_;
    usize cursor_;
    bool truncated_ = false;
};

LogCaptureValue EncodeScalar(const FormatArg& arg) noexcept {
    LogCaptureValue value;
    value.kind = static_cast<uint8>(arg.kind);
    switch (arg.kind) {
    case FormatArgKind::Bool:
        value.payload = arg.boolean ? 1u : 0u;
        break;
    case FormatArgKind::Char:
        value.payload = static_cast<uint8>(arg.character);
        break;
    case FormatArgKind::Signed:
        value.payload = static_cast<uint64>(arg.signedValue);
        break;
    case FormatArgKind::Unsigned:
        value.payload = arg.unsignedValue;
        break;
    case FormatArgKind::Float:
        value.payload = platform::BitCast<uint32>(arg.single);
        break;
    case FormatArgKind::Double:
        value.payload = platform::BitCast<uint64>(arg.real);
        break;
    case FormatArgKind::Pointer:
        value.payload = static_cast<uint64>(reinterpret_cast<uptr>(arg.pointer));
        break;
    case FormatArgKind::Text:
    case FormatArgKind::NullText:
    case FormatArgKind::Custom:
        break;
    }

    return value;
}

// 읽기: 캡처 머리와 값 칸을 꺼내고 텍스트 위치를 확인한다.
class CaptureReader {
public:
    CaptureReader(Span<const uint8> capture, const LogBlockProvider* blocks) noexcept
        : capture_(capture), blocks_(blocks) {
    }

    bool Open() noexcept {
        if (capture_.Size() < sizeof(LogCaptureHeader)) {
            return false;
        }

        platform::MemoryCopy(&header_, capture_.Data(), sizeof(header_));
        if (header_.magic != CaptureMagic || header_.totalBytes > capture_.Size() ||
            header_.valueCount > MaxLogArguments ||
            LogCaptureMinimumBytes(header_.valueCount) > header_.totalBytes ||
            header_.category == nullptr) {
            return false;
        }

        platform::MemoryCopy(values_, capture_.Data() + sizeof(LogCaptureHeader),
                             header_.valueCount * sizeof(LogCaptureValue));
        for (usize index = 0; index < header_.valueCount; ++index) {
            if (values_[index].kind > static_cast<uint8>(FormatArgKind::Custom)) {
                return false;
            }
        }

        return true;
    }

    const LogCaptureHeader& Header() const noexcept {
        return header_;
    }

    const LogCaptureValue& Value(usize index) const noexcept {
        return values_[index];
    }

    // 텍스트 값의 byte다. 범위를 벗어나거나 블록을 찾을 수 없으면 false다.
    bool Text(const LogCaptureValue& value, StringView& text) const noexcept {
        if ((value.flags & ValueInBlock) != 0) {
            if (blocks_ == nullptr) {
                return false;
            }

            const Span<const uint8> block = blocks_->Resolve(static_cast<uint32>(value.payload));
            if (value.bytes > block.Size()) {
                return false;
            }

            text = StringView(reinterpret_cast<const char*>(block.Data()), value.bytes);
            return true;
        }

        if (value.payload > header_.totalBytes ||
            value.bytes > header_.totalBytes - value.payload) {
            return false;
        }

        text =
            StringView(reinterpret_cast<const char*>(capture_.Data() + value.payload), value.bytes);
        return true;
    }

    bool TemplateText(StringView& text) const noexcept {
        if ((header_.flags & CaptureRuntimeTemplate) != 0) {
            return Text(header_.runtimeTemplate, text);
        }

        if (header_.literalTemplate == nullptr && header_.templateBytes != 0) {
            return false;
        }

        text = StringView(header_.literalTemplate, header_.templateBytes);
        return true;
    }

private:
    Span<const uint8> capture_;
    const LogBlockProvider* blocks_;
    LogCaptureHeader header_{};
    LogCaptureValue values_[MaxLogArguments]{};
};

// 텍스트가 아닌 값 칸을 서식 인자로 되살린다. 텍스트 종류는 text를 쓴다.
FormatArg DecodeArg(const LogCaptureValue& value, StringView text) noexcept {
    FormatArg arg;
    arg.kind = static_cast<FormatArgKind>(value.kind);
    switch (arg.kind) {
    case FormatArgKind::Bool:
        arg.boolean = value.payload != 0;
        break;
    case FormatArgKind::Char:
        arg.character = static_cast<char>(static_cast<uint8>(value.payload));
        break;
    case FormatArgKind::Signed:
        arg.signedValue = static_cast<int64>(value.payload);
        break;
    case FormatArgKind::Unsigned:
        arg.unsignedValue = value.payload;
        break;
    case FormatArgKind::Float:
        arg.single = platform::BitCast<float>(static_cast<uint32>(value.payload));
        break;
    case FormatArgKind::Double:
        arg.real = platform::BitCast<double>(value.payload);
        break;
    case FormatArgKind::Pointer:
        arg.pointer = reinterpret_cast<const void*>(static_cast<uptr>(value.payload));
        break;
    case FormatArgKind::Text:
    case FormatArgKind::Custom:
        arg.kind = FormatArgKind::Text;
        arg.text.data = text.Data();
        arg.text.bytes = text.ByteCount();
        break;
    case FormatArgKind::NullText:
        break;
    }

    return arg;
}

// 자리 하나를 렌더한다. 실패하면 그 자리 출력을 지우고 "<format error>"를 붙인다.
bool RenderHole(FixedStringBuilder& out, const CaptureReader& reader, const LogCaptureValue& value,
                const detail::FormatToken& token) noexcept {
    const usize start = out.ByteCount();
    const bool truncated = out.Truncated();
    bool rendered = false;
    StringView text;
    const auto kind = static_cast<FormatArgKind>(value.kind);
    if (kind == FormatArgKind::Custom) {
        // 캡처 때 spec까지 적용해 렌더한 텍스트다. 그대로 붙인다.
        rendered = (value.flags & ValueFormatError) == 0 && reader.Text(value, text) &&
                   out.TryAppend(text).IsOk();
    } else if (kind != FormatArgKind::NullText) {
        const bool hasText = kind != FormatArgKind::Text || reader.Text(value, text);
        rendered = hasText && detail::AppendFormatArgument(out, DecodeArg(value, text), token.spec,
                                                           token.hasSpec)
                                  .IsOk();
    }

    if (rendered) {
        return true;
    }

    out.RewindTo(start, truncated);
    (void)out.TryAppend(FormatErrorText);
    return false;
}

bool IsNonFinite(uint64 doubleBits) noexcept {
    constexpr uint64 ExponentMask = 0x7FF0000000000000ull;
    return (doubleBits & ExponentMask) == ExponentMask;
}

StringView NonFiniteText(uint64 doubleBits) noexcept {
    constexpr uint64 MantissaMask = 0x000FFFFFFFFFFFFFull;
    constexpr uint64 SignBit = 0x8000000000000000ull;
    if ((doubleBits & MantissaMask) != 0) {
        return "nan";
    }

    return (doubleBits & SignBit) != 0 ? StringView("-inf") : StringView("inf");
}

// 속성 하나를 채운다. 숫자·char·포인터의 텍스트는 texts에 기본 서식으로 쓴다.
// 반환은 서식 실패가 있었는지다.
bool FillProperty(const CaptureReader& reader, const LogCaptureValue& value,
                  FixedStringBuilder& texts, LogProperty& property) noexcept {
    const auto kind = static_cast<FormatArgKind>(value.kind);
    StringView text;
    property.kind = LogPropertyKind::String;
    property.signedValue = 0;
    switch (kind) {
    case FormatArgKind::Bool:
        property.kind = LogPropertyKind::Bool;
        property.boolValue = value.payload != 0;
        property.text = value.payload != 0 ? StringView("true") : StringView("false");
        return false;
    case FormatArgKind::Text:
        if (!reader.Text(value, text) || !text.IsValidUtf8()) {
            property.text = FormatErrorText;
            return true;
        }

        property.text = text;
        return false;
    case FormatArgKind::Custom:
        if ((value.flags & ValueFormatError) != 0 || !reader.Text(value, text)) {
            property.text = FormatErrorText;
            return true;
        }

        property.text = text;
        return false;
    case FormatArgKind::NullText:
        property.text = FormatErrorText;
        return true;
    case FormatArgKind::Float:
    case FormatArgKind::Double: {
        const uint64 bits =
            kind == FormatArgKind::Double
                ? value.payload
                : detail::decimal::FloatBitsToDoubleBits(static_cast<uint32>(value.payload));
        if (IsNonFinite(bits)) {
            property.text = NonFiniteText(bits);
            return false;
        }

        property.kind = LogPropertyKind::Float;
        property.floatValue = platform::BitCast<double>(bits);
        break;
    }
    case FormatArgKind::Signed:
        property.kind = LogPropertyKind::Signed;
        property.signedValue = static_cast<int64>(value.payload);
        break;
    case FormatArgKind::Unsigned:
        property.kind = LogPropertyKind::Unsigned;
        property.unsignedValue = value.payload;
        break;
    case FormatArgKind::Char:
    case FormatArgKind::Pointer:
        break;
    }

    // 숫자·char·포인터는 기본 서식 텍스트를 texts 영역에 쓴다.
    const usize start = texts.ByteCount();
    const bool truncated = texts.Truncated();
    if (!detail::AppendFormatArgument(texts, DecodeArg(value, StringView()), StringView(), false)) {
        texts.RewindTo(start, truncated);
        property.text = FormatErrorText;
        return true;
    }

    property.text = StringView(texts.View().Data() + start, texts.ByteCount() - start);
    return false;
}

} // namespace

namespace detail {

Result<usize> WriteLogCapture(const LogCaptureTarget& target, LogLevel level,
                              const LogCategory& category, const LogCaptureTemplate& text,
                              const FormatArg* args, usize count) noexcept {
    if (count > MaxLogArguments || !IsValidLogLevel(level)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize fixedBytes = LogCaptureMinimumBytes(count);
    if (target.buffer.Size() < fixedBytes ||
        target.buffer.Size() > static_cast<usize>(NumericLimits<uint32>::Max())) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    LogCaptureHeader header;
    header.magic = CaptureMagic;
    header.valueCount = static_cast<uint16>(count);
    header.level = static_cast<uint8>(level);
    header.category = &category;
    header.eventId = text.eventId;
    header.location = text.location;
    header.monotonicNanoseconds = target.monotonicNanoseconds;
    header.threadId = target.threadId;
    header.frame = target.frame;

    CaptureWriter writer(target, fixedBytes);
    if (text.runtime) {
        header.flags |= CaptureRuntimeTemplate;
        header.runtimeTemplate.kind = static_cast<uint8>(FormatArgKind::Text);
        writer.StoreText(text.text, header.runtimeTemplate);
    } else {
        header.literalTemplate = text.text.Data();
        header.templateBytes = static_cast<uint32>(text.text.ByteCount());
    }

    // 사용자 Formatter 인자가 있으면 그 인자를 처음 쓴 자리의 spec을 찾는다(템플릿이 잘못됐으면
    // 오류 앞까지만 찾고 나머지는 빈 spec).
    LogTemplateLayout layout;
    bool hasCustom = false;
    FormatArgKind kinds[MaxLogArguments]{};
    for (usize index = 0; index < count; ++index) {
        kinds[index] = args[index].kind;
        hasCustom = hasCustom || args[index].kind == FormatArgKind::Custom;
    }

    if (hasCustom) {
        (void)AnalyzeLogTemplate(text.text, kinds, count, layout);
    }

    LogCaptureValue values[MaxLogArguments];
    for (usize index = 0; index < count; ++index) {
        const FormatArg& arg = args[index];
        if (arg.kind == FormatArgKind::Text) {
            values[index].kind = static_cast<uint8>(FormatArgKind::Text);
            writer.StoreText(StringView(arg.text.data, arg.text.bytes), values[index]);
        } else if (arg.kind == FormatArgKind::Custom) {
            writer.StoreCustom(arg, layout.specs[index], values[index]);
        } else {
            values[index] = EncodeScalar(arg);
        }
    }

    if (writer.Truncated()) {
        header.flags |= CaptureValueTruncated;
    }

    header.totalBytes = static_cast<uint32>(writer.Cursor());
    platform::MemoryCopy(target.buffer.Data(), &header, sizeof(header));
    if (count != 0) {
        platform::MemoryCopy(target.buffer.Data() + sizeof(header), values,
                             count * sizeof(LogCaptureValue));
    }

    return Result<usize>::Success(writer.Cursor());
}

} // namespace detail

Result<void> RenderLogCapture(Span<const uint8> capture, const LogBlockProvider* blocks,
                              const LogRenderTarget& target, LogEvent& event) noexcept {
    CaptureReader reader(capture, blocks);
    StringView templateText;
    if (target.message.Size() == 0 || !reader.Open() || !reader.TemplateText(templateText)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const LogCaptureHeader& header = reader.Header();
    const usize count = header.valueCount;
    LogEventFlags flags = LogEventFlags::None;
    if ((header.flags & CaptureValueTruncated) != 0) {
        flags = flags | LogEventFlags::ValueTruncated;
    }

    // 리터럴 템플릿도 같은 검증을 거친다(컴파일 타임에 통과했으므로 늘 성공한다).
    FormatArgKind kinds[MaxLogArguments]{};
    for (usize index = 0; index < count; ++index) {
        kinds[index] = static_cast<FormatArgKind>(reader.Value(index).kind);
    }

    detail::LogTemplateLayout layout;
    const LogTemplateError error = detail::AnalyzeLogTemplate(templateText, kinds, count, layout);

    FixedStringBuilder message(target.message);
    if (error != LogTemplateError::None) {
        // 원본 템플릿을 그대로 두고 오류 이름을 덧붙인다. 원본이 UTF-8이 아니면 원본은 뺀다.
        if (message.TryAppend(templateText).IsOk() && !templateText.IsEmpty()) {
            (void)message.TryAppendAscii(' ');
        }

        (void)message.TryAppend("[template error: ");
        (void)message.TryAppend(LogTemplateErrorName(error));
        (void)message.TryAppendAscii(']');
        flags = flags | LogEventFlags::TemplateError;
    } else {
        detail::FormatParser parser(templateText, true);
        detail::FormatToken token;
        while (parser.Next(token)) {
            if (token.kind == detail::FormatTokenKind::Literal) {
                (void)message.TryAppend(token.text);
                continue;
            }

            if (!RenderHole(message, reader, reader.Value(token.argument), token)) {
                flags = flags | LogEventFlags::FormatError;
            }
        }
    }

    // 속성: 인자마다 하나. 이름 자리면 그 이름, 아니면 Arg0…
    const usize filled = count < target.properties.Size() ? count : target.properties.Size();
    char emptyTexts[1];
    FixedStringBuilder texts(target.propertyText.Size() != 0 ? target.propertyText
                                                             : Span<char>(emptyTexts));
    for (usize index = 0; index < filled; ++index) {
        LogProperty& property = target.properties[index];
        property.name = error == LogTemplateError::None && layout.named
                            ? layout.names[index]
                            : PositionalPropertyNames[index];
        if (FillProperty(reader, reader.Value(index), texts, property)) {
            flags = flags | LogEventFlags::FormatError;
        }
    }

    if (message.Truncated() || texts.Truncated() || filled < count) {
        flags = flags | LogEventFlags::MessageTruncated;
    }

    event.level = static_cast<LogLevel>(header.level);
    event.category = header.category->Name();
    event.messageTemplate = templateText;
    event.eventId = header.eventId;
    event.message = message.View();
    event.properties = Span<const LogProperty>(target.properties.Data(), filled);
    event.location = header.location;
    event.threadId = header.threadId;
    event.frame = header.frame;
    event.monotonicNanoseconds = header.monotonicNanoseconds;
    event.flags = flags;
    return Result<void>::Success();
}

void ReleaseLogCaptureBlocks(Span<const uint8> capture, LogBlockProvider& blocks) noexcept {
    CaptureReader reader(capture, &blocks);
    if (!reader.Open()) {
        return;
    }

    const LogCaptureHeader& header = reader.Header();
    if ((header.flags & CaptureRuntimeTemplate) != 0 &&
        (header.runtimeTemplate.flags & ValueInBlock) != 0) {
        blocks.Release(static_cast<uint32>(header.runtimeTemplate.payload));
    }

    for (usize index = 0; index < header.valueCount; ++index) {
        const LogCaptureValue& value = reader.Value(index);
        if ((value.flags & ValueInBlock) != 0) {
            blocks.Release(static_cast<uint32>(value.payload));
        }
    }
}

} // namespace rocks
