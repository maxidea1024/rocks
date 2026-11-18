#include <Config/ConfigTypes.h>

#include <Base/NumericLimits.h>
#include <Platform/Fatal.h>

namespace rocks::config {

StringView ConfigLayerName(ConfigLayer layer) noexcept {
    switch (layer) {
    case ConfigLayer::CodeDefault:
        return "CodeDefault";
    case ConfigLayer::Engine:
        return "Engine";
    case ConfigLayer::EnginePlatform:
        return "EnginePlatform";
    case ConfigLayer::Project:
        return "Project";
    case ConfigLayer::ProjectPlatform:
        return "ProjectPlatform";
    case ConfigLayer::Feature:
        return "Feature";
    case ConfigLayer::Quality:
        return "Quality";
    case ConfigLayer::DeviceProfile:
        return "DeviceProfile";
    case ConfigLayer::User:
        return "User";
    case ConfigLayer::Session:
        return "Session";
    case ConfigLayer::Console:
        return "Console";
    }

    return {};
}

StringView ConfigDiagnosticCodeName(ConfigDiagnosticCode code) noexcept {
    switch (code) {
    case ConfigDiagnosticCode::JsonSyntax:
        return "JsonSyntax";
    case ConfigDiagnosticCode::UnknownBranch:
        return "UnknownBranch";
    case ConfigDiagnosticCode::UnknownSection:
        return "UnknownSection";
    case ConfigDiagnosticCode::InvalidSection:
        return "InvalidSection";
    case ConfigDiagnosticCode::FieldValue:
        return "FieldValue";
    case ConfigDiagnosticCode::InvalidArrayOperation:
        return "InvalidArrayOperation";
    case ConfigDiagnosticCode::MisplacedOperation:
        return "MisplacedOperation";
    case ConfigDiagnosticCode::UnsupportedOperation:
        return "UnsupportedOperation";
    case ConfigDiagnosticCode::InvalidPathToken:
        return "InvalidPathToken";
    case ConfigDiagnosticCode::LayerNotAllowed:
        return "LayerNotAllowed";
    case ConfigDiagnosticCode::InvalidSetting:
        return "InvalidSetting";
    case ConfigDiagnosticCode::UnknownTarget:
        return "UnknownTarget";
    case ConfigDiagnosticCode::ConsoleReadOnly:
        return "ConsoleReadOnly";
    case ConfigDiagnosticCode::NotQualityMember:
        return "NotQualityMember";
    case ConfigDiagnosticCode::ValueOutOfRange:
        return "ValueOutOfRange";
    case ConfigDiagnosticCode::WeakWrite:
        return "WeakWrite";
    case ConfigDiagnosticCode::UserFileVersion:
        return "UserFileVersion";
    case ConfigDiagnosticCode::UnknownSetting:
        return "UnknownSetting";
    case ConfigDiagnosticCode::RestartRequired:
        return "RestartRequired";
    case ConfigDiagnosticCode::BootValueIgnored:
        return "BootValueIgnored";
    case ConfigDiagnosticCode::InvalidDeviceProfile:
        return "InvalidDeviceProfile";
    }

    return {};
}

bool IsConsoleSettingName(StringView name) noexcept {
    usize segments = 0;
    bool start = true;
    for (const char c : name) {
        if (c == '.') {
            if (start) {
                return false;
            }

            start = true;
            continue;
        }

        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (start ? !lower : !(lower || digit || c == '_')) {
            return false;
        }

        if (start) {
            ++segments;
        }

        start = false;
    }

    return !start && segments >= 2;
}

ConfigDiagnostics::ConfigDiagnostics(Allocator& allocator) noexcept
    : text_(allocator), entries_(allocator) {
}

void ConfigDiagnostics::SetLimit(uint32 maxDiagnostics) noexcept {
    if (maxDiagnostics == 0) {
        platform::Fatal("ConfigDiagnostics::SetLimit requires a positive limit");
    }

    limit_ = maxDiagnostics;
}

Result<void> ConfigDiagnostics::Add(const ConfigDiagnostic& diagnostic) noexcept {
    const bool known = !ConfigDiagnosticCodeName(diagnostic.code).IsEmpty() &&
                       (diagnostic.severity == ConfigSeverity::Error ||
                        diagnostic.severity == ConfigSeverity::Warning ||
                        diagnostic.severity == ConfigSeverity::Note) &&
                       IsConfigLayer(diagnostic.layer);
    if (!known) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (entries_.Size() >= limit_) {
        if (omitted_ != NumericLimits<uint64>::Max()) {
            ++omitted_;
        }

        return Result<void>::Success();
    }

    // 텍스트를 먼저 붙이고 레코드를 더한다. 레코드 추가가 실패하면 텍스트를 되돌린다(이전 진단의
    // view는 Get으로 다시 얻으므로 버퍼 재할당과 무관하다).
    const usize offset = text_.Size();
    Result<void> appended =
        text_.TryAppend(Span<const char>(diagnostic.source.Data(), diagnostic.source.ByteCount()));
    if (appended) {
        appended =
            text_.TryAppend(Span<const char>(diagnostic.path.Data(), diagnostic.path.ByteCount()));
    }

    if (appended) {
        appended = text_.TryAppend(
            Span<const char>(diagnostic.message.Data(), diagnostic.message.ByteCount()));
    }

    Entry entry{};
    entry.head = diagnostic;
    entry.textOffset = offset;
    entry.sourceBytes = diagnostic.source.ByteCount();
    entry.pathBytes = diagnostic.path.ByteCount();
    entry.messageBytes = diagnostic.message.ByteCount();
    if (appended) {
        appended = entries_.TryPushBack(entry);
    }

    if (!appended) {
        static_cast<void>(text_.TryResize(offset)); // 줄이기는 할당하지 않는다
        return appended;
    }

    if (diagnostic.severity == ConfigSeverity::Error) {
        ++errors_;
    }

    return Result<void>::Success();
}

ConfigDiagnostic ConfigDiagnostics::Get(usize index) const noexcept {
    if (index >= entries_.Size()) {
        platform::Fatal("ConfigDiagnostics::Get index out of range");
    }

    const Entry& entry = entries_[index];
    ConfigDiagnostic result = entry.head;
    const char* base = text_.Data() + entry.textOffset;
    result.source = StringView(base, entry.sourceBytes);
    result.path = StringView(base + entry.sourceBytes, entry.pathBytes);
    result.message = StringView(base + entry.sourceBytes + entry.pathBytes, entry.messageBytes);
    return result;
}

void ConfigDiagnostics::Clear() noexcept {
    text_.Clear();
    entries_.Clear();
    omitted_ = 0;
    errors_ = 0;
}

ConfigDiagnosticsMark ConfigDiagnostics::Mark() const noexcept {
    return ConfigDiagnosticsMark{entries_.Size(), errors_, omitted_, text_.Size()};
}

void ConfigDiagnostics::Rewind(const ConfigDiagnosticsMark& mark) noexcept {
    if (mark.count > entries_.Size() || mark.textBytes > text_.Size() || mark.errors > errors_ ||
        mark.omitted > omitted_) {
        platform::Fatal("ConfigDiagnostics::Rewind with a mark from after the current state");
    }

    // 줄이기는 할당하지 않는다.
    static_cast<void>(entries_.TryResize(mark.count));
    static_cast<void>(text_.TryResize(mark.textBytes));
    errors_ = mark.errors;
    omitted_ = mark.omitted;
}

} // namespace rocks::config
