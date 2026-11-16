#include <Core/Text/Json.h>

#include "JsonInternal.h"
#include <Platform/Memory.h>

namespace rocks {

using detail::HashKeyBytes;
using detail::Overlaps;

// ---- 쓰기 ----

uint64 JsonWriter::WrittenKeyHash::operator()(const WrittenKey& key) const noexcept {
    return HashKeyBytes(bytes, key.object, out->View().Data() + key.offset, key.length);
}

bool JsonWriter::WrittenKeyEqual::operator()(const WrittenKey& left,
                                             const WrittenKey& right) const noexcept {
    const char* data = out->View().Data();
    return left.object == right.object && left.length == right.length &&
           platform::MemoryCompare(data + left.offset, data + right.offset, left.length) == 0;
}

JsonWriter::JsonWriter(StringBuilderBase& out, JsonNumberMode mode, uint32 maxDepth) noexcept
    : out_(&out), mode_(mode), maxDepth_(maxDepth), frames_(out.GetAllocator()),
      keys_(out.GetAllocator(), WrittenKeyHash{&out, {}}, WrittenKeyEqual{&out}) {
}

Result<void> JsonWriter::Fail(ErrorCode code) noexcept {
    failed_ = true;
    return Result<void>::Failure(code);
}

Result<void> JsonWriter::Check(Result<void> result) noexcept {
    if (!result) {
        failed_ = true;
    }

    return result;
}

Result<void> JsonWriter::WriteIndent(usize depth) noexcept {
    if (auto newline = Check(out_->TryAppendAscii('\n')); !newline) {
        return newline;
    }

    return Check(out_->TryAppendRepeated(' ', depth * 2));
}

// 값 앞의 구분자(쉼표·줄바꿈·들여쓰기)를 쓴다.
Result<void> JsonWriter::BeforeValue() noexcept {
    if (failed_ || finished_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (frames_.IsEmpty()) {
        return rootWritten_ ? Fail(ErrorCode::InvalidArgument) : Result<void>::Success();
    }

    Frame& top = frames_[frames_.Size() - 1];
    if (top.object) {
        // 객체: Key 뒤에만 값이 온다(구분자는 Key가 썼다).
        if (!expectingValue_) {
            return Fail(ErrorCode::InvalidArgument);
        }

        expectingValue_ = false;
        return Result<void>::Success();
    }

    const bool first = !top.hasItems;
    top.hasItems = true;
    if (!first) {
        if (auto comma = Check(out_->TryAppendAscii(',')); !comma) {
            return comma;
        }
    }

    return WriteIndent(frames_.Size());
}

Result<void> JsonWriter::AfterValue() noexcept {
    if (frames_.IsEmpty()) {
        rootWritten_ = true;
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::WriteEscaped(StringView text) noexcept {
    // 입력이 out의 View()이면 쓰는 동안 주소가 바뀔 수 있으므로 먼저 복사한다.
    Array<char> copy(out_->GetAllocator());
    if (Overlaps(text.Data(), text.ByteCount(), out_->View().Data(), out_->ByteCount())) {
        if (!copy.TryAppend(Span<const char>(text.Data(), text.ByteCount()))) {
            return Fail(ErrorCode::OutOfMemory);
        }

        text = StringView(copy.Data(), copy.Size());
    }

    if (!text.ValidateUtf8()) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto quote = Check(out_->TryAppendAscii('"')); !quote) {
        return quote;
    }

    const char* data = text.Data();
    usize start = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        const uint8 byte = index < text.ByteCount() ? static_cast<uint8>(data[index]) : 0;
        const bool needsEscape =
            index < text.ByteCount() && (byte == '"' || byte == '\\' || byte < 0x20);
        if (!needsEscape && index < text.ByteCount()) {
            continue;
        }

        if (index > start) {
            if (auto run = Check(out_->TryAppend(StringView(data + start, index - start))); !run) {
                return run;
            }
        }

        if (index == text.ByteCount()) {
            break;
        }

        char escape[6] = {'\\', 0, 0, 0, 0, 0};
        usize length = 2;
        switch (byte) {
        case '"':
            escape[1] = '"';
            break;
        case '\\':
            escape[1] = '\\';
            break;
        case '\b':
            escape[1] = 'b';
            break;
        case '\f':
            escape[1] = 'f';
            break;
        case '\n':
            escape[1] = 'n';
            break;
        case '\r':
            escape[1] = 'r';
            break;
        case '\t':
            escape[1] = 't';
            break;
        default: {
            constexpr char Hex[] = "0123456789abcdef";
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = Hex[byte >> 4];
            escape[5] = Hex[byte & 0xF];
            length = 6;
            break;
        }
        }

        if (auto written = Check(out_->TryAppend(StringView(escape, length))); !written) {
            return written;
        }

        start = index + 1;
    }

    return Check(out_->TryAppendAscii('"'));
}

Result<void> JsonWriter::BeginContainer(bool object) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (frames_.Size() >= maxDepth_) {
        return Fail(ErrorCode::CapacityExceeded);
    }

    if (auto open = Check(out_->TryAppendAscii(object ? '{' : '[')); !open) {
        return open;
    }

    if (!frames_.TryPushBack(Frame{object, false, object ? nextSerial_++ : 0})) {
        return Fail(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::EndContainer(bool object) noexcept {
    if (failed_ || finished_ || frames_.IsEmpty() || frames_[frames_.Size() - 1].object != object ||
        expectingValue_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    const bool hasItems = frames_[frames_.Size() - 1].hasItems;
    static_cast<void>(frames_.TryResize(frames_.Size() - 1)); // 줄이기는 할당하지 않는다
    if (hasItems) {
        if (auto indent = WriteIndent(frames_.Size()); !indent) {
            return indent;
        }
    }

    if (auto close = Check(out_->TryAppendAscii(object ? '}' : ']')); !close) {
        return close;
    }

    return AfterValue();
}

Result<void> JsonWriter::BeginObject() noexcept {
    return BeginContainer(true);
}

Result<void> JsonWriter::BeginArray() noexcept {
    return BeginContainer(false);
}

Result<void> JsonWriter::EndObject() noexcept {
    return EndContainer(true);
}

Result<void> JsonWriter::EndArray() noexcept {
    return EndContainer(false);
}

Result<void> JsonWriter::Key(StringView key) noexcept {
    if (failed_ || finished_ || frames_.IsEmpty() || !frames_[frames_.Size() - 1].object ||
        expectingValue_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    Frame& top = frames_[frames_.Size() - 1];
    const bool first = !top.hasItems;
    const uint32 serial = top.serial;
    top.hasItems = true;
    if (!first) {
        if (auto comma = Check(out_->TryAppendAscii(',')); !comma) {
            return comma;
        }
    }

    if (auto indent = WriteIndent(frames_.Size()); !indent) {
        return indent;
    }

    const usize start = out_->ByteCount();
    if (auto written = WriteEscaped(key); !written) {
        return written;
    }

    // 같은 객체의 같은 키는 자기 reader가 거부하므로 쓰지 않는다(이스케이프가 결정적이라
    // 이스케이프된 byte로 비교한다).
    auto inserted =
        keys_.Insert(WrittenKey{serial, start, out_->ByteCount() - start}, static_cast<uint8>(0));
    if (!inserted) {
        return Fail(ErrorCode::OutOfMemory);
    }

    if (inserted.Value() == InsertOutcome::AlreadyPresent) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto separator = Check(out_->TryAppend(StringView(": "))); !separator) {
        return separator;
    }

    expectingValue_ = true;
    return Result<void>::Success();
}

Result<void> JsonWriter::Null() noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppend(StringView("null"))); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Bool(bool value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppend(StringView(value ? "true" : "false"))); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Int64(int64 value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::UInt64(uint64 value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::Double(double value) noexcept {
    // NaN·Inf는 JSON에 없다(비트로 판정 — 부동소수점 환경과 무관).
    if ((platform::BitCast<uint64>(value) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto before = BeforeValue(); !before) {
        return before;
    }

    const usize start = out_->ByteCount();
    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return FinishFloatingPoint(start);
}

Result<void> JsonWriter::Float(float value) noexcept {
    // NaN·Inf는 JSON에 없다(비트로 판정 — 부동소수점 환경과 무관).
    if ((platform::BitCast<uint32>(value) & 0x7F800000u) == 0x7F800000u) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto before = BeforeValue(); !before) {
        return before;
    }

    // StringBuilder의 float "{}"는 단정도 최단 왕복 표기이고 고정·과학 표기 경계가 double과 같다.
    const usize start = out_->ByteCount();
    if (auto written = Check(out_->TryAppendFormat("{}", value)); !written) {
        return written;
    }

    return FinishFloatingPoint(start);
}

// start부터 쓴 최단 실수 표기를 마무리한다.
Result<void> JsonWriter::FinishFloatingPoint(usize start) noexcept {
    // 최단 표기에 '.'·'e'가 없으면 ".0"을 붙인다(−0도 "-0" → "-0.0"). 다시 읽어도 double이다.
    const StringView text = out_->View();
    bool hasMarker = false;
    for (usize index = start; index < text.ByteCount(); ++index) {
        const char character = text.Data()[index];
        hasMarker = hasMarker || character == '.' || character == 'e' || character == 'E';
    }

    if (!hasMarker) {
        if (auto suffix = Check(out_->TryAppend(StringView(".0"))); !suffix) {
            return suffix;
        }
    }

    return AfterValue();
}

Result<void> JsonWriter::String(StringView value) noexcept {
    if (auto before = BeforeValue(); !before) {
        return before;
    }

    if (auto written = WriteEscaped(value); !written) {
        return written;
    }

    return AfterValue();
}

Result<void> JsonWriter::WriteNumber(JsonValue value) noexcept {
    if (mode_ == JsonNumberMode::Preserve) {
        if (auto before = BeforeValue(); !before) {
            return before;
        }

        if (auto written = Check(out_->TryAppend(value.RawNumber())); !written) {
            return written;
        }

        return AfterValue();
    }

    switch (value.Type()) {
    case JsonType::Integer:
        return Int64(value.TryGetInt64().Value());
    case JsonType::Unsigned:
        return UInt64(value.TryGetUInt64().Value());
    default:
        return Double(value.TryGetDouble().Value());
    }
}

Result<void> JsonWriter::Value(JsonValue value) noexcept {
    if (!value.IsValid()) {
        return Fail(ErrorCode::InvalidArgument);
    }

    // 명시 스택으로 쓴다: 컨테이너를 열면 (컨테이너, 다음 자식 위치)를 쌓는다.
    struct Pending {
        JsonValue container;
        usize next;
    };
    Array<Pending> stack(out_->GetAllocator());
    auto emit = [this, &stack](JsonValue item) noexcept -> Result<void> {
        switch (item.Type()) {
        case JsonType::Null:
            return Null();
        case JsonType::Bool:
            return Bool(item.GetBool());
        case JsonType::Integer:
        case JsonType::Unsigned:
        case JsonType::Double:
            return WriteNumber(item);
        case JsonType::String:
            return String(item.GetString());
        case JsonType::Array:
        case JsonType::Object: {
            if (auto begin = BeginContainer(item.Type() == JsonType::Object); !begin) {
                return begin;
            }

            if (!stack.TryPushBack(Pending{item, 0})) {
                return Fail(ErrorCode::OutOfMemory);
            }

            return Result<void>::Success();
        }
        }

        return Fail(ErrorCode::InvalidArgument);
    };
    if (auto first = emit(value); !first) {
        return first;
    }

    while (!stack.IsEmpty()) {
        Pending& top = stack[stack.Size() - 1];
        const JsonValue container = top.container;
        if (top.next == container.Size()) {
            static_cast<void>(stack.TryResize(stack.Size() - 1));
            if (auto end = EndContainer(container.Type() == JsonType::Object); !end) {
                return end;
            }

            continue;
        }

        const usize index = top.next++;
        JsonValue child;
        if (container.Type() == JsonType::Object) {
            if (auto key = Key(container.KeyAt(index)); !key) {
                return key;
            }

            child = container.ValueAt(index);
        } else {
            child = container.At(index);
        }

        if (auto written = emit(child); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

Result<void> JsonWriter::Finish() noexcept {
    if (failed_ || finished_ || !frames_.IsEmpty() || !rootWritten_) {
        return Fail(ErrorCode::InvalidArgument);
    }

    if (auto newline = Check(out_->TryAppendAscii('\n')); !newline) {
        return newline;
    }

    finished_ = true;
    return Result<void>::Success();
}

} // namespace rocks
