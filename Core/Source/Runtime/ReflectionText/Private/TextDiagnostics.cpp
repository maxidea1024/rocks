#include <ReflectionText/TextDiagnostics.h>

#include <Base/NumericLimits.h>
#include <Platform/Fatal.h>

namespace rocks::reflection_text {

namespace {

bool IsValidSourceKind(TextSourceKind kind) noexcept {
    switch (kind) {
    case TextSourceKind::None:
    case TextSourceKind::Assignment:
    case TextSourceKind::Json:
        return true;
    }

    return false;
}

} // namespace

StringView TextDiagnosticCodeName(TextDiagnosticCode code) noexcept {
    switch (code) {
    case TextDiagnosticCode::UnknownProperty:
        return "UnknownProperty";
    case TextDiagnosticCode::ExcludedProperty:
        return "ExcludedProperty";
    case TextDiagnosticCode::ReadOnly:
        return "ReadOnly";
    case TextDiagnosticCode::InvalidValue:
        return "InvalidValue";
    case TextDiagnosticCode::OutOfRange:
        return "OutOfRange";
    case TextDiagnosticCode::DuplicateKey:
        return "DuplicateKey";
    case TextDiagnosticCode::DuplicateAssignment:
        return "DuplicateAssignment";
    case TextDiagnosticCode::HiddenEnumerator:
        return "HiddenEnumerator";
    case TextDiagnosticCode::UnknownType:
        return "UnknownType";
    case TextDiagnosticCode::Unresolved:
        return "Unresolved";
    case TextDiagnosticCode::Unsupported:
        return "Unsupported";
    case TextDiagnosticCode::InvalidStructValue:
        return "InvalidStructValue";
    }

    return {};
}

TextDiagnostics::TextDiagnostics(Allocator& allocator) noexcept
    : text_(allocator), entries_(allocator) {
}

void TextDiagnostics::SetLimit(uint32 maxDiagnostics) noexcept {
    if (maxDiagnostics == 0) {
        platform::Fatal("TextDiagnostics::SetLimit: limit must be at least 1");
    }

    limit_ = maxDiagnostics;
}

Result<void> TextDiagnostics::Add(TextDiagnosticCode code, TextSource source, StringView path,
                                  StringView message) noexcept {
    if (TextDiagnosticCodeName(code).IsEmpty() || !IsValidSourceKind(source.kind)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (Full()) {
        if (omitted_ != NumericLimits<uint64>::Max()) {
            ++omitted_;
        }

        return Result<void>::Success();
    }

    // 텍스트 버퍼와 레코드 버퍼를 먼저 모두 확보한 뒤 붙인다(둘째 확보가 실패해도 내용은 그대로다).
    const usize used = text_.ByteCount();
    if (path.ByteCount() > NumericLimits<usize>::Max() - used ||
        message.ByteCount() > NumericLimits<usize>::Max() - used - path.ByteCount()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const usize required = used + path.ByteCount() + message.ByteCount();
    if (required > text_.ByteCapacity()) {
        // 진단마다 재할당하지 않게 두 배로 늘린다(넘치면 필요한 만큼만).
        const usize capacity = text_.ByteCapacity();
        const usize doubled = capacity > NumericLimits<usize>::Max() / 2 ? required : capacity * 2;
        auto reserved = text_.TryReserve(doubled > required ? doubled : required);
        if (!reserved) {
            return reserved;
        }
    }

    auto entry = entries_.TryEnsureCapacity(entries_.Size() + 1);
    if (!entry) {
        return entry;
    }

    // 용량을 확보했으므로 아래 붙이기는 실패하지 않는다.
    if (!text_.TryAppend(path) || !text_.TryAppend(message)) {
        platform::Fatal("TextDiagnostics::Add: append failed after reserve");
    }

    if (!entries_.TryPushBack(Entry{code, source, used, path.ByteCount(), message.ByteCount()})) {
        platform::Fatal("TextDiagnostics::Add: push failed after reserve");
    }

    return Result<void>::Success();
}

TextDiagnostic TextDiagnostics::Get(usize index) const noexcept {
    if (index >= entries_.Size()) {
        platform::Fatal("TextDiagnostics::Get: index out of range");
    }

    const Entry& entry = entries_[index];
    const char* text = text_.Data();
    TextDiagnostic diagnostic;
    diagnostic.code = entry.code;
    diagnostic.source = entry.source;
    diagnostic.path = StringView(text + entry.textOffset, entry.pathBytes);
    diagnostic.message = StringView(text + entry.textOffset + entry.pathBytes, entry.messageBytes);
    return diagnostic;
}

void TextDiagnostics::Clear() noexcept {
    text_.Clear();
    entries_.Clear();
    omitted_ = 0;
}

} // namespace rocks::reflection_text
