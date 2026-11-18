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

Result<void> WriteProperty(JsonWriter& json, const LogProperty& property) noexcept {
    if (auto key = json.Key(property.name); !key) {
        return key;
    }

    switch (property.kind) {
    case LogPropertyKind::Signed:
        return json.Int64(property.signedValue);
    case LogPropertyKind::Unsigned:
        return json.UInt64(property.unsignedValue);
    case LogPropertyKind::Float:
        return json.Double(property.floatValue);
    case LogPropertyKind::Bool:
        return json.Bool(property.boolValue);
    case LogPropertyKind::String:
        break;
    }

    return json.String(property.text);
}

// 한 줄 전체를 쓴다. 실패하면(버퍼 넘침은 호출자가 Truncated로 본다) 오류를 돌려준다.
Result<void> WriteRecord(JsonWriter& json, const LogEvent& event) noexcept {
    char scratch[48];
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
        result = WriteProperty(json, event.properties[index]);
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

    // 상관 id: 0이 아닌 칸만, 비었으면 키를 쓰지 않는다.
    if (result && event.hasCorrelation) {
        const struct {
            StringView name;
            uint64 value;
        } fields[] = {{"session", event.correlation.session},
                      {"task", event.correlation.task},
                      {"asset", event.correlation.asset},
                      {"entity", event.correlation.entity},
                      {"request", event.correlation.request}};
        result = json.Key("corr");
        if (result) {
            result = json.BeginObject();
        }

        for (usize index = 0; result && index < sizeof(fields) / sizeof(fields[0]); ++index) {
            if (fields[index].value == 0) {
                continue;
            }

            result = json.Key(fields[index].name);
            if (result) {
                result = json.UInt64(fields[index].value);
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
    const usize room = capacity > FallbackReserveBytes ? (capacity - FallbackReserveBytes) / 6 : 0;
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
