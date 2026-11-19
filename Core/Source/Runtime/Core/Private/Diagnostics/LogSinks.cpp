#include <Core/Diagnostics/LogStructure.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace rocks {
namespace {

// sink가 받는 event의 기본 검사다(손으로 만든 LogEvent가 줄바꿈 등으로 가짜 줄을 만들지 못하게).
bool IsWritableEvent(const LogEvent& event) noexcept {
    return IsValidLogLevel(event.level) &&
           ValidateLogCategoryName(event.category) == LogCategoryNameError::None &&
           event.message.IsValidUtf8();
}

} // namespace

// ---- TextLogSink ----

void TextLogSink::WriteBuffered() noexcept {
    if (used_ != 0 && !output_.write(output_.context, StringView(buffer_, used_))) {
        failed_ = true;
    }

    used_ = 0;
}

void TextLogSink::Append(void* context, StringView text) noexcept {
    auto& sink = *static_cast<TextLogSink*>(context);
    if (text.ByteCount() > BufferBytes - sink.used_) {
        sink.WriteBuffered();
    }

    // 버퍼보다 큰 조각(긴 메시지)은 모으지 않고 바로 쓴다.
    if (text.ByteCount() > BufferBytes) {
        if (!sink.output_.write(sink.output_.context, text)) {
            sink.failed_ = true;
        }

        return;
    }

    platform::MemoryCopy(sink.buffer_ + sink.used_, text.Data(), text.ByteCount());
    sink.used_ += text.ByteCount();
}

Result<void> TextLogSink::Write(const LogEvent& event) noexcept {
    if (!IsWritableEvent(event) || output_.write == nullptr || output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(mutex_);
    used_ = 0;
    failed_ = false;
    layout_.Render(event, LogLayoutOutput{this, Append});
    WriteBuffered();
    return failed_ ? Result<void>::Failure(ErrorCode::DependencyFailed) : Result<void>::Success();
}

Result<void> TextLogSink::Flush() noexcept {
    if (output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(mutex_);
    return output_.flush(output_.context) ? Result<void>::Success()
                                          : Result<void>::Failure(ErrorCode::DependencyFailed);
}

// ---- StderrLogSink ----

namespace {

Result<void> WriteStderr(void*, StringView bytes) noexcept {
    if ((bytes.ByteCount() != 0 &&
         fwrite(bytes.Data(), 1, bytes.ByteCount(), stderr) != bytes.ByteCount()) ||
        ferror(stderr) != 0) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

Result<void> FlushStderr(void*) noexcept {
    return fflush(stderr) == 0 && ferror(stderr) == 0
               ? Result<void>::Success()
               : Result<void>::Failure(ErrorCode::DependencyFailed);
}

} // namespace

StderrLogSink::StderrLogSink(const LogLayout& layout) noexcept
    : sink_({nullptr, WriteStderr, FlushStderr}, layout) {
}

Result<void> StderrLogSink::Write(const LogEvent& event) noexcept {
    return sink_.Write(event);
}

Result<void> StderrLogSink::Flush() noexcept {
    return sink_.Flush();
}

// ---- JsonLinesLogSink ----

namespace {

struct FlagName {
    LogEventFlags flag;
    StringView name;
};

constexpr FlagName FlagNames[] = {
    {LogEventFlags::MessageTruncated, "MessageTruncated"},
    {LogEventFlags::ContextTruncated, "ContextTruncated"},
    {LogEventFlags::ValueTruncated, "ValueTruncated"},
    {LogEventFlags::TemplateError, "TemplateError"},
    {LogEventFlags::FormatError, "FormatError"},
};

// 대체 줄에 남길 메시지 byte 수의 여유분이다(다른 필드·이스케이프 자리).
constexpr usize FallbackReserveBytes = 1024;

// 메시지 byte 하나가 JSON 문자열에서 차지할 수 있는 최대 byte 수다(제어 문자 "\u00XX").
constexpr usize JsonEscapedBytesPerByte = 6;

// 숫자·시각 필드를 서식화하는 임시 버퍼 크기다(UTC 시각 문자열과 64-bit 정수가 들어간다).
constexpr usize FieldScratchBytes = 48;

// 펼친 값 노드 트리의 최대 깊이다(맨 위 속성 + 펼친 값 깊이). 손으로 만든 LogEvent가 더 깊으면
// 쓰지 않는다(재귀 깊이를 묶는다).
constexpr usize MaxJsonStructureDepth = MaxLogStructureDepth + 1;

Result<void> WriteValue(JsonWriter& json, const LogProperty& property,
                        Span<const LogProperty> nodes, usize depth) noexcept;

// Structure·Array 노드의 직접 자식을 차례로 부른다. 범위가 structureNodes 밖이거나 자식 하위
// 트리가 부모 범위를 넘으면 InvalidArgument다(대체 줄로 간다).
template <typename Visit>
Result<void> ForEachChild(const LogProperty& node, Span<const LogProperty> nodes,
                          Visit&& visit) noexcept {
    const usize begin = node.subtreeBegin;
    const usize end = begin + node.subtreeCount;
    if (begin > nodes.Size() || node.subtreeCount > nodes.Size() - begin) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = begin; index < end;) {
        const LogProperty& child = nodes[index];
        const usize span = LogPropertySubtreeSpan(child);
        if (span > end - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto visited = visit(child); !visited) {
            return visited;
        }

        index += span;
    }

    return Result<void>::Success();
}

// 펼친 객체는 JSON 객체다: 타입 태그는 "$type", 잘림은 "$truncated": true.
Result<void> WriteStructure(JsonWriter& json, const LogProperty& property,
                            Span<const LogProperty> nodes, usize depth) noexcept {
    Result<void> result = json.BeginObject();
    if (result && !property.typeTag.IsEmpty()) {
        result = json.Key("$type");
        if (result) {
            result = json.String(property.typeTag);
        }
    }

    if (result) {
        result = ForEachChild(property, nodes, [&](const LogProperty& child) noexcept {
            if (auto key = json.Key(child.name); !key) {
                return key;
            }

            return WriteValue(json, child, nodes, depth + 1);
        });
    }

    if (result && property.truncated) {
        result = json.Key("$truncated");
        if (result) {
            result = json.Bool(true);
        }
    }

    if (!result) {
        return result;
    }

    return json.EndObject();
}

// 펼친 배열은 JSON 배열이다. 원소를 버렸으면(잘림) 전체 개수를 함께 남기려고
// {"$count": 전체 원소 수, "$items": [담은 원소]}로 낸다.
Result<void> WriteArray(JsonWriter& json, const LogProperty& property,
                        Span<const LogProperty> nodes, usize depth) noexcept {
    Result<void> result = Result<void>::Success();
    if (property.truncated) {
        result = json.BeginObject();
        if (result) {
            result = json.Key("$count");
        }

        if (result) {
            result = json.UInt64(property.unsignedValue);
        }

        if (result) {
            result = json.Key("$items");
        }
    }

    if (result) {
        result = json.BeginArray();
    }

    if (result) {
        result = ForEachChild(property, nodes, [&](const LogProperty& child) noexcept {
            return WriteValue(json, child, nodes, depth + 1);
        });
    }

    if (result) {
        result = json.EndArray();
    }

    if (result && property.truncated) {
        result = json.EndObject();
    }

    return result;
}

Result<void> WriteValue(JsonWriter& json, const LogProperty& property,
                        Span<const LogProperty> nodes, usize depth) noexcept {
    switch (property.kind) {
    case LogPropertyKind::Signed:
        return json.Int64(property.signedValue);
    case LogPropertyKind::Unsigned:
        return json.UInt64(property.unsignedValue);
    case LogPropertyKind::Float:
        return json.Double(property.floatValue);
    case LogPropertyKind::Bool:
        return json.Bool(property.boolValue);
    case LogPropertyKind::Null:
        return json.Null();
    case LogPropertyKind::Structure:
    case LogPropertyKind::Array:
        if (depth >= MaxJsonStructureDepth) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return property.kind == LogPropertyKind::Structure
                   ? WriteStructure(json, property, nodes, depth)
                   : WriteArray(json, property, nodes, depth);
    case LogPropertyKind::String:
        break;
    }

    return json.String(property.text);
}

Result<void> WriteProperty(JsonWriter& json, const LogProperty& property,
                           Span<const LogProperty> nodes) noexcept {
    if (auto key = json.Key(property.name); !key) {
        return key;
    }

    return WriteValue(json, property, nodes, 0);
}

// 한 줄 전체를 쓴다. 실패하면(버퍼 넘침은 호출자가 Truncated로 본다) 오류를 돌려준다.
Result<void> WriteRecord(JsonWriter& json, const LogEvent& event) noexcept {
    char scratch[FieldScratchBytes];
    Result<void> result = json.BeginObject();
    if (result) {
        result = json.Key("t");
    }

    if (result) {
        auto time = DateTime::TryFromTicks(event.utcTicks);
        if (event.utcTicks > 0 && time) {
            FixedStringBuilder text{Span<char>(scratch)};
            result = text.TryAppendFormat("{:ms}", time.Value());
            if (result) {
                result = json.String(text.View());
            }
        } else {
            result = json.Null();
        }
    }

    if (result) {
        result = json.Key("mono");
    }

    if (result) {
        result = json.UInt64(event.monotonicNanoseconds);
    }

    if (result) {
        result = json.Key("lvl");
    }

    if (result) {
        result = json.String(LogLevelName(event.level));
    }

    if (result) {
        result = json.Key("cat");
    }

    if (result) {
        result = json.String(event.category);
    }

    if (result) {
        result = json.Key("event");
    }

    if (result) {
        FixedStringBuilder text{Span<char>(scratch)};
        result = text.TryAppendFormat("{:016x}", event.eventId);
        if (result) {
            result = json.String(text.View());
        }
    }

    if (result) {
        result = json.Key("tpl");
    }

    if (result) {
        result = json.String(event.messageTemplate);
    }

    if (result) {
        result = json.Key("msg");
    }

    if (result) {
        result = json.String(event.message);
    }

    if (result) {
        result = json.Key("props");
    }

    if (result) {
        result = json.BeginObject();
    }

    for (usize index = 0; result && index < event.properties.Size(); ++index) {
        result = WriteProperty(json, event.properties[index], event.structureNodes);
    }

    if (result) {
        result = json.EndObject();
    }

    if (result) {
        result = json.Key("ctx");
    }

    if (result) {
        result = json.BeginObject();
    }

    for (usize index = 0; result && index < event.context.Size(); ++index) {
        result = json.Key(event.context[index].name);
        if (result) {
            result = json.String(event.context[index].text);
        }
    }

    if (result) {
        result = json.EndObject();
    }

    // 상관 id: 0이 아닌 칸만, 모두 0이거나 없으면 키를 쓰지 않는다.
    if (result && event.hasCorrelation && !IsEmptyCorrelation(event.correlation)) {
        uint64 values[CorrelationFieldCount];
        CorrelationFieldValues(event.correlation, values);
        result = json.Key("corr");
        if (result) {
            result = json.BeginObject();
        }

        for (usize index = 0; result && index < CorrelationFieldCount; ++index) {
            if (values[index] == 0) {
                continue;
            }

            result = json.Key(CorrelationFieldNames[index]);
            if (result) {
                result = json.UInt64(values[index]);
            }
        }

        if (result) {
            result = json.EndObject();
        }
    }

    if (result) {
        result = json.Key("src");
    }

    if (result) {
        if (event.location.file == nullptr || event.location.file[0] == '\0') {
            result = json.Null();
        } else {
            result = json.BeginObject();
            if (result) {
                result = json.Key("file");
            }

            if (result) {
                result = json.String(StringView(event.location.file));
            }

            if (result) {
                result = json.Key("line");
            }

            if (result) {
                result = json.UInt64(event.location.line);
            }

            if (result) {
                result = json.Key("func");
            }

            if (result) {
                result = json.String(StringView(event.location.function));
            }

            if (result) {
                result = json.EndObject();
            }
        }
    }

    if (result) {
        result = json.Key("thread");
    }

    if (result) {
        if (!event.threadName.IsEmpty()) {
            result = json.String(event.threadName);
        } else {
            FixedStringBuilder text{Span<char>(scratch)};
            result = text.TryAppendFormat("{}", event.threadId);
            if (result) {
                result = json.String(text.View());
            }
        }
    }

    if (result) {
        result = json.Key("frame");
    }

    if (result) {
        result = json.UInt64(event.frame);
    }

    if (result) {
        result = json.Key("flags");
    }

    if (result) {
        result = json.BeginArray();
    }

    for (const FlagName& flag : FlagNames) {
        if (result && HasAnyLogEventFlags(event.flags, flag.flag)) {
            result = json.String(flag.name);
        }
    }

    if (result) {
        result = json.EndArray();
    }

    if (result) {
        result = json.EndObject();
    }

    if (result) {
        result = json.Finish();
    }

    return result;
}

// 버퍼를 넘은 기록의 대체 줄이다. 메시지를 버퍼에 들어가게 잘라 필수 필드만 쓴다.
Result<void> WriteFallbackRecord(JsonWriter& json, const LogEvent& event, usize capacity) noexcept {
    const usize room = capacity > FallbackReserveBytes
                           ? (capacity - FallbackReserveBytes) / JsonEscapedBytesPerByte
                           : 0;
    Result<void> result = json.BeginObject();
    if (result) {
        result = json.Key("mono");
    }

    if (result) {
        result = json.UInt64(event.monotonicNanoseconds);
    }

    if (result) {
        result = json.Key("lvl");
    }

    if (result) {
        result = json.String(LogLevelName(event.level));
    }

    if (result) {
        result = json.Key("cat");
    }

    if (result) {
        result = json.String(event.category);
    }

    if (result) {
        result = json.Key("msg");
    }

    // 이스케이프가 한 byte를 최대 6 byte(제어 문자 이스케이프)로 늘리므로 그만큼 여유를 둔다.
    if (result) {
        result = json.String(event.message.LeftBytesAtCodePoint(room));
    }

    if (result) {
        result = json.Key("flags");
    }

    if (result) {
        result = json.BeginArray();
    }

    if (result) {
        result = json.String("MessageTruncated");
    }

    if (result) {
        result = json.EndArray();
    }

    if (result) {
        result = json.EndObject();
    }

    if (result) {
        result = json.Finish();
    }

    return result;
}

JsonWriterOptions LineOptions() noexcept {
    JsonWriterOptions options;
    options.compact = true;
    options.fixedFrames = true;
    options.detectDuplicateKeys = false;
    return options;
}

} // namespace

Result<void> JsonLinesLogSink::Write(const LogEvent& event) noexcept {
    if (!IsWritableEvent(event) || output_.write == nullptr || output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(mutex_);
    FixedStringBuilder line{Span<char>(buffer_)};
    {
        JsonWriter json(line, LineOptions());
        const auto written = WriteRecord(json, event);
        if (written && !line.Truncated()) {
            return output_.write(output_.context, line.View())
                       ? Result<void>::Success()
                       : Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    // 넘쳤거나(잘림) 쓸 수 없는 값이 있었다: 늘 유효한 대체 줄을 쓴다.
    line.Clear();
    JsonWriter json(line, LineOptions());
    if (!WriteFallbackRecord(json, event, line.Capacity()) || line.Truncated()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return output_.write(output_.context, line.View())
               ? Result<void>::Success()
               : Result<void>::Failure(ErrorCode::DependencyFailed);
}

Result<void> JsonLinesLogSink::Flush() noexcept {
    if (output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(mutex_);
    return output_.flush(output_.context) ? Result<void>::Success()
                                          : Result<void>::Failure(ErrorCode::DependencyFailed);
}

} // namespace rocks
