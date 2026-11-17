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
constexpr uint8 CaptureContextTruncated = 1u << 2;

using detail::LogCaptureContext;

// 값 칸 count개와 문맥 칸 contexts개를 담은 캡처 앞부분의 크기다.
constexpr usize FixedCaptureBytes(usize count, usize contexts) noexcept {
    return LogCaptureMinimumBytes(count) + contexts * sizeof(LogCaptureContext);
}

// LogCaptureValue::flags
constexpr uint8 ValueInBlock = 1u << 0;
constexpr uint8 ValueTruncated = 1u << 1;
constexpr uint8 ValueFormatError = 1u << 2;

constexpr StringView FormatErrorText = "<format error>";
constexpr StringView TemplateErrorPrefix = " [template error: ";
constexpr StringView TemplateErrorSuffix = "]";

static_assert(MaxLogArguments <= NumericLimits<uint16>::Max(), "value count is stored as uint16");

// 위치 자리의 속성 이름이다(Arg0…). MaxLogArguments개를 미리 둔다.
constexpr StringView PositionalPropertyNames[] = {
    "Arg0", "Arg1", "Arg2",  "Arg3",  "Arg4",  "Arg5",  "Arg6",  "Arg7",
    "Arg8", "Arg9", "Arg10", "Arg11", "Arg12", "Arg13", "Arg14", "Arg15"};

static_assert(sizeof(PositionalPropertyNames) / sizeof(PositionalPropertyNames[0]) ==
                  MaxLogArguments,
              "every argument position needs a property name");

Span<char> BlockChars(const LogBlock& block) noexcept {
    return Span<char>(reinterpret_cast<char*>(block.bytes.Data()), block.bytes.Size());
}

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

    // 사용자 Formatter 결과를 렌더한다. Formatter는 한 번만 부른다.
    // - 남은 영역이 블록보다 작으면 블록에 먼저 렌더하고, 결과가 남은 영역에 들어가면 옮긴 뒤
    //   블록을 돌려준다.
    // - 블록이 없거나(provider 없음·고갈) 너무 작으면 남은 영역에 렌더한다(넘치면 잘림).
    void StoreCustom(const FormatArg& arg, StringView spec, LogCaptureValue& value) noexcept {
        value.kind = static_cast<uint8>(FormatArgKind::Custom);
        LogBlock block;
        if (target_.blocks != nullptr && Remaining() < target_.blocks->BlockBytes() &&
            target_.blocks->TryBorrow(block)) {
            if (block.bytes.Size() >= MinimumLogRenderBytes) {
                RenderIntoBlock(arg, spec, block, value);
                return;
            }

            // 계약보다 작은 블록은 쓰지 않는다(장애 격리: 생성자의 Fatal로 가지 않는다).
            target_.blocks->Release(block.index);
        }

        if (Remaining() < MinimumLogRenderBytes) {
            value.payload = cursor_;
            value.bytes = 0;
            MarkTruncated(value, true);
            return;
        }

        FixedStringBuilder text(Span<char>(InlineData(), Remaining()));
        if (!arg.custom.format(text, arg.custom.object, spec)) {
            value.flags |= ValueFormatError;
            return;
        }

        value.payload = cursor_;
        value.bytes = static_cast<uint32>(text.ByteCount());
        cursor_ += text.ByteCount();
        MarkTruncated(value, text.Truncated());
    }

private:
    void RenderIntoBlock(const FormatArg& arg, StringView spec, const LogBlock& block,
                         LogCaptureValue& value) noexcept {
        FixedStringBuilder text(BlockChars(block));
        if (!arg.custom.format(text, arg.custom.object, spec)) {
            target_.blocks->Release(block.index);
            value.flags |= ValueFormatError;
            return;
        }

        // 남은 영역에 들어가면 옮기고 블록을 바로 돌려준다(블록은 큰 값에만 남긴다).
        if (!text.Truncated() && text.ByteCount() <= Remaining()) {
            StoreInline(text.View(), value);
            target_.blocks->Release(block.index);
            return;
        }

        value.flags |= ValueInBlock;
        value.payload = block.index;
        value.bytes = static_cast<uint32>(text.ByteCount());
        MarkTruncated(value, text.Truncated());
    }

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

bool IsTextValue(const LogCaptureValue& value) noexcept {
    const auto kind = static_cast<FormatArgKind>(value.kind);
    return kind == FormatArgKind::Text ||
           (kind == FormatArgKind::Custom && (value.flags & ValueFormatError) == 0);
}

// 읽기: 캡처 머리와 값 칸을 꺼내고 텍스트 위치를 확인한다.
class CaptureReader {
public:
    CaptureReader(Span<const uint8> capture, const LogBlockProvider* blocks) noexcept
        : capture_(capture), blocks_(blocks) {
    }

    // 머리와 값 칸의 형식(표지·길이·값 수·종류·레벨·카테고리 길이)을 검사한다.
    bool Open() noexcept {
        if (capture_.Size() < sizeof(LogCaptureHeader)) {
            return false;
        }

        platform::MemoryCopy(&header_, capture_.Data(), sizeof(header_));
        if (header_.magic != CaptureMagic || header_.totalBytes > capture_.Size() ||
            header_.valueCount > MaxLogArguments || header_.contextCount > MaxLogContextDepth ||
            FixedCaptureBytes(header_.valueCount, header_.contextCount) > header_.totalBytes ||
            !IsValidLogLevel(static_cast<LogLevel>(header_.level)) ||
            header_.categoryBytes > MaxLogCategoryNameBytes ||
            (header_.categoryName == nullptr && header_.categoryBytes != 0)) {
            return false;
        }

        platform::MemoryCopy(values_, capture_.Data() + sizeof(LogCaptureHeader),
                             header_.valueCount * sizeof(LogCaptureValue));
        for (usize index = 0; index < header_.valueCount; ++index) {
            if (values_[index].kind > static_cast<uint8>(FormatArgKind::Custom)) {
                return false;
            }
        }

        platform::MemoryCopy(contexts_,
                             capture_.Data() + LogCaptureMinimumBytes(header_.valueCount),
                             header_.contextCount * sizeof(LogCaptureContext));
        for (usize index = 0; index < header_.contextCount; ++index) {
            if (contexts_[index].name == nullptr ||
                contexts_[index].value.kind != static_cast<uint8>(FormatArgKind::Text)) {
                return false;
            }
        }

        return true;
    }

    // 모든 텍스트 값(런타임 템플릿 포함)의 위치가 캡처·블록 안인지 검사한다. 렌더 전에 한 번
    // 부른다.
    bool ValidateTexts() const noexcept {
        StringView text;
        for (usize index = 0; index < header_.valueCount; ++index) {
            if (IsTextValue(values_[index]) && !Text(values_[index], text)) {
                return false;
            }
        }

        for (usize index = 0; index < header_.contextCount; ++index) {
            if (!Text(contexts_[index].value, text)) {
                return false;
            }
        }

        return TemplateText(text);
    }

    const LogCaptureContext& Context(usize index) const noexcept {
        return contexts_[index];
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
            if (blocks_ == nullptr || value.payload > NumericLimits<uint32>::Max()) {
                return false;
            }

            const Span<const uint8> block = blocks_->Resolve(static_cast<uint32>(value.payload));
            if (block.Size() == 0 || value.bytes > block.Size()) {
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

    bool IsRuntimeTemplate() const noexcept {
        return (header_.flags & CaptureRuntimeTemplate) != 0;
    }

    bool TemplateText(StringView& text) const noexcept {
        if (IsRuntimeTemplate()) {
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
    LogCaptureContext contexts_[MaxLogContextDepth]{};
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

    const usize bytes = texts.ByteCount() - start;
    property.text = bytes == 0 ? StringView() : StringView(texts.View().Data() + start, bytes);
    return false;
}

// 템플릿 오류: 원본을 남기고 오류 이름을 붙인다. 메시지가 모자라면 원본을 잘라 오류 표기를 남긴다.
void RenderTemplateError(FixedStringBuilder& message, StringView templateText,
                         LogTemplateError error, LogEventFlags& flags) noexcept {
    const StringView name = LogTemplateErrorName(error);
    const usize suffixBytes =
        TemplateErrorPrefix.ByteCount() + name.ByteCount() + TemplateErrorSuffix.ByteCount();
    const usize room = message.Capacity() > suffixBytes ? message.Capacity() - suffixBytes : 0;
    // 원본이 UTF-8이 아니면 원본은 뺀다(InvalidUtf8 오류).
    if (templateText.IsValidUtf8()) {
        const StringView kept = templateText.LeftBytesAtCodePoint(room);
        (void)message.TryAppend(kept);
        if (kept.ByteCount() < templateText.ByteCount()) {
            flags = flags | LogEventFlags::MessageTruncated;
        }
    }

    (void)message.TryAppend(TemplateErrorPrefix);
    (void)message.TryAppend(name);
    (void)message.TryAppend(TemplateErrorSuffix);
    flags = flags | LogEventFlags::TemplateError;
}

} // namespace

namespace detail {

Result<usize> WriteLogCapture(const LogCaptureTarget& target, LogLevel level,
                              const LogCategory& category, const LogCaptureTemplate& text,
                              const FormatArg* args, usize count) noexcept {
    if (count > MaxLogArguments || !IsValidLogLevel(level)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (target.buffer.Size() < LogCaptureMinimumBytes(count) ||
        target.buffer.Size() > static_cast<usize>(NumericLimits<uint32>::Max())) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // 이 스레드의 문맥(LogContextScope)을 담는다. 문맥 칸이 슬롯에 다 들어가지 않으면 안쪽부터
    // 버린다.
    bool contextTruncated = false;
    const Span<const LogContextEntry> context = CurrentLogContext(contextTruncated);
    usize contextCount = context.Size();
    while (contextCount != 0 && target.buffer.Size() < FixedCaptureBytes(count, contextCount)) {
        --contextCount;
        contextTruncated = true;
    }

    const usize fixedBytes = FixedCaptureBytes(count, contextCount);

    const StringView categoryName = category.Name();
    LogCaptureHeader header;
    header.magic = CaptureMagic;
    header.valueCount = static_cast<uint16>(count);
    header.level = static_cast<uint8>(level);
    header.categoryBytes = static_cast<uint8>(categoryName.ByteCount());
    header.categoryName = categoryName.Data();
    header.categoryHash = category.NameHash();
    header.eventId = text.eventId;
    header.location = text.location;
    header.monotonicNanoseconds = target.monotonicNanoseconds;
    header.threadId = target.threadId;
    header.frame = target.frame;
    header.contextCount = static_cast<uint8>(contextCount);
    if (contextTruncated) {
        header.flags |= CaptureContextTruncated;
    }

    CaptureWriter writer(target, fixedBytes);
    if (text.runtime) {
        header.flags |= CaptureRuntimeTemplate;
        header.runtimeTemplate.kind = static_cast<uint8>(FormatArgKind::Text);
        writer.StoreText(text.text, header.runtimeTemplate);
    } else {
        header.literalTemplate = text.text.Data();
        header.templateBytes = static_cast<uint32>(text.text.ByteCount());
    }

    // 사용자 Formatter 인자의 spec: 리터럴 템플릿은 컴파일 타임에 계산한 위치를 쓰고, 런타임
    // 템플릿만 여기서 해석한다(템플릿이 잘못됐으면 오류 앞까지만 찾고 나머지는 빈 spec).
    bool hasCustom = false;
    FormatArgKind kinds[MaxLogArguments]{};
    for (usize index = 0; index < count; ++index) {
        kinds[index] = args[index].kind;
        hasCustom = hasCustom || args[index].kind == FormatArgKind::Custom;
    }

    LogTemplateLayout layout;
    if (hasCustom && text.specs == nullptr) {
        (void)AnalyzeLogTemplate(text.text, kinds, count, layout);
    }

    LogCaptureValue values[MaxLogArguments];
    for (usize index = 0; index < count; ++index) {
        const FormatArg& arg = args[index];
        if (arg.kind == FormatArgKind::Text) {
            values[index].kind = static_cast<uint8>(FormatArgKind::Text);
            writer.StoreText(StringView(arg.text.data, arg.text.bytes), values[index]);
        } else if (arg.kind == FormatArgKind::Custom) {
            StringView spec = layout.specs[index];
            if (text.specs != nullptr) {
                spec = StringView(text.text.Data() + text.specs[index].offset,
                                  text.specs[index].bytes);
            }

            writer.StoreCustom(arg, spec, values[index]);
        } else {
            values[index] = EncodeScalar(arg);
        }
    }

    LogCaptureContext contexts[MaxLogContextDepth];
    for (usize index = 0; index < contextCount; ++index) {
        contexts[index].name = context[index].name;
        contexts[index].nameBytes = static_cast<uint32>(context[index].nameBytes);
        contexts[index].value.kind = static_cast<uint8>(FormatArgKind::Text);
        writer.StoreText(context[index].value, contexts[index].value);
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

    if (contextCount != 0) {
        platform::MemoryCopy(target.buffer.Data() + LogCaptureMinimumBytes(count), contexts,
                             contextCount * sizeof(LogCaptureContext));
    }

    return Result<usize>::Success(writer.Cursor());
}

} // namespace detail

Result<void> RenderLogCapture(Span<const uint8> capture, const LogBlockProvider* blocks,
                              const LogRenderTarget& target, LogEvent& event) noexcept {
    CaptureReader reader(capture, blocks);
    StringView templateText;
    if (target.message.Size() == 0 || !reader.Open() || !reader.ValidateTexts() ||
        !reader.TemplateText(templateText)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const LogCaptureHeader& header = reader.Header();
    const usize count = header.valueCount;
    LogEventFlags flags = LogEventFlags::None;
    if ((header.flags & CaptureValueTruncated) != 0) {
        flags = flags | LogEventFlags::ValueTruncated;
    }

    FixedStringBuilder message(target.message);
    detail::LogTemplateLayout layout;
    LogTemplateError error = LogTemplateError::None;
    const bool templateTruncated =
        reader.IsRuntimeTemplate() && (header.runtimeTemplate.flags & ValueTruncated) != 0;
    if (templateTruncated) {
        // 캡처 때 잘린 런타임 템플릿은 해석하면 거짓 오류가 난다. 원본(잘린 것)을 그대로 쓴다.
        if (templateText.IsValidUtf8()) {
            (void)message.TryAppend(templateText);
        }

        flags = flags | LogEventFlags::MessageTruncated;
    } else {
        // 리터럴 템플릿도 같은 검증을 거친다(컴파일 타임에 통과했으므로 늘 성공한다).
        detail::FormatArgKind kinds[MaxLogArguments]{};
        for (usize index = 0; index < count; ++index) {
            kinds[index] = static_cast<detail::FormatArgKind>(reader.Value(index).kind);
        }

        error = detail::AnalyzeLogTemplate(templateText, kinds, count, layout);
        if (error != LogTemplateError::None) {
            RenderTemplateError(message, templateText, error, flags);
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
    }

    // 속성: 인자마다 하나. 이름 자리면 그 이름, 아니면 Arg0…
    const bool named = !templateTruncated && error == LogTemplateError::None && layout.named;
    const usize filled = count < target.properties.Size() ? count : target.properties.Size();
    char emptyTexts[1];
    FixedStringBuilder texts(target.propertyText.Size() != 0 ? target.propertyText
                                                             : Span<char>(emptyTexts));
    for (usize index = 0; index < filled; ++index) {
        LogProperty& property = target.properties[index];
        property.name = named ? layout.names[index] : PositionalPropertyNames[index];
        if (FillProperty(reader, reader.Value(index), texts, property)) {
            flags = flags | LogEventFlags::FormatError;
        }
    }

    if (message.Truncated() || texts.Truncated() || filled < count) {
        flags = flags | LogEventFlags::MessageTruncated;
    }

    // 문맥: 이름=값. 값은 캡처 때 복사한 텍스트다.
    const usize contextCount = header.contextCount;
    const usize contextFilled =
        contextCount < target.context.Size() ? contextCount : target.context.Size();
    for (usize index = 0; index < contextFilled; ++index) {
        const LogCaptureContext& entry = reader.Context(index);
        LogProperty& property = target.context[index];
        StringView value;
        (void)reader.Text(entry.value, value); // ValidateTexts가 이미 확인했다
        property.name = StringView(entry.name, entry.nameBytes);
        property.text = value.IsValidUtf8() ? value : FormatErrorText;
        property.kind = LogPropertyKind::String;
        property.signedValue = 0;
    }

    if ((header.flags & CaptureContextTruncated) != 0 ||
        (contextFilled < contextCount && target.context.Size() != 0)) {
        flags = flags | LogEventFlags::ContextTruncated;
    }

    event.level = static_cast<LogLevel>(header.level);
    event.category = StringView(header.categoryName, header.categoryBytes);
    event.messageTemplate = templateText;
    event.eventId = header.eventId;
    event.message = message.View();
    event.properties = Span<const LogProperty>(target.properties.Data(), filled);
    event.context = Span<const LogProperty>(target.context.Data(), contextFilled);
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
    if (reader.IsRuntimeTemplate() && (header.runtimeTemplate.flags & ValueInBlock) != 0) {
        blocks.Release(static_cast<uint32>(header.runtimeTemplate.payload));
    }

    for (usize index = 0; index < header.valueCount; ++index) {
        const LogCaptureValue& value = reader.Value(index);
        if ((value.flags & ValueInBlock) != 0) {
            blocks.Release(static_cast<uint32>(value.payload));
        }
    }

    for (usize index = 0; index < header.contextCount; ++index) {
        const LogCaptureValue& value = reader.Context(index).value;
        if ((value.flags & ValueInBlock) != 0) {
            blocks.Release(static_cast<uint32>(value.payload));
        }
    }
}

// ---- 문맥 스택(스레드별, 힙 없음) ----

namespace detail {
namespace {

thread_local LogContextEntry contextStack[MaxLogContextDepth];
thread_local usize contextDepth = 0;

} // namespace

Span<const LogContextEntry> CurrentLogContext(bool& truncated) noexcept {
    truncated = contextDepth > MaxLogContextDepth;
    const usize stored = contextDepth < MaxLogContextDepth ? contextDepth : MaxLogContextDepth;
    return Span<const LogContextEntry>(contextStack, stored);
}

void PushLogContext(const char* name, usize nameBytes, StringView value) noexcept {
    // 깊이를 넘는 범위는 담지 않고 깊이만 센다(그 범위가 열린 동안 ContextTruncated).
    if (contextDepth < MaxLogContextDepth) {
        contextStack[contextDepth] = LogContextEntry{name, nameBytes, value};
    }

    ++contextDepth;
}

void PopLogContext() noexcept {
    if (contextDepth != 0) {
        --contextDepth;
    }
}

} // namespace detail

} // namespace rocks
