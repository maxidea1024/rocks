#include <UnrealExchange/ExchangeDiagnostic.h>

// Unreal 교환 진단 목록이다(Docs/Design/UnrealExchange.ko.md 10.3·10.4절 3번).

namespace rocks {

const char* ExchangeIssueName(ExchangeIssue issue) noexcept {
    switch (issue) {
    case ExchangeIssue::None:
        return "None";
    case ExchangeIssue::ReadFailed:
        return "ReadFailed";
    case ExchangeIssue::JsonSyntax:
        return "JsonSyntax";
    case ExchangeIssue::ByteOrderMark:
        return "ByteOrderMark";
    case ExchangeIssue::MissingKey:
        return "MissingKey";
    case ExchangeIssue::WrongType:
        return "WrongType";
    case ExchangeIssue::InvalidValue:
        return "InvalidValue";
    case ExchangeIssue::UnknownKey:
        return "UnknownKey";
    case ExchangeIssue::NotSorted:
        return "NotSorted";
    case ExchangeIssue::ObjectPathSyntax:
        return "ObjectPathSyntax";
    case ExchangeIssue::FileNameMismatch:
        return "FileNameMismatch";
    case ExchangeIssue::ManifestMismatch:
        return "ManifestMismatch";
    case ExchangeIssue::RedirectConflict:
        return "RedirectConflict";
    case ExchangeIssue::BlobConflict:
        return "BlobConflict";
    case ExchangeIssue::BlobSize:
        return "BlobSize";
    case ExchangeIssue::BlobHash:
        return "BlobHash";
    case ExchangeIssue::BlobLayout:
        return "BlobLayout";
    case ExchangeIssue::LimitExceeded:
        return "LimitExceeded";
    case ExchangeIssue::Version:
        return "Version";
    case ExchangeIssue::UnsupportedClass:
        return "UnsupportedClass";
    case ExchangeIssue::Renamed:
        return "Renamed";
    case ExchangeIssue::PathTooLong:
        return "PathTooLong";
    case ExchangeIssue::PathCollision:
        return "PathCollision";
    case ExchangeIssue::InvalidRoot:
        return "InvalidRoot";
    case ExchangeIssue::BuildCycle:
        return "BuildCycle";
    case ExchangeIssue::DepthLimit:
        return "DepthLimit";
    case ExchangeIssue::MissingTarget:
        return "MissingTarget";
    case ExchangeIssue::FailedDependency:
        return "FailedDependency";
    case ExchangeIssue::FailedMember:
        return "FailedMember";
    case ExchangeIssue::NotRegularFile:
        return "NotRegularFile";
    case ExchangeIssue::ExporterError:
        return "ExporterError";
    }

    return "Unknown";
}

ExchangeSeverity ExchangeSeverityOf(AssetDiagnosticCode code) noexcept {
    switch (code) {
    case AssetDiagnosticCode::UnknownField:
    case AssetDiagnosticCode::UnsupportedAsset:
    case AssetDiagnosticCode::MissingUnrealDependency:
        return ExchangeSeverity::Warning;
    case AssetDiagnosticCode::UnrealPathRenamed:
        return ExchangeSeverity::Info;
    default:
        return ExchangeSeverity::Error;
    }
}

ExchangeDiagnostics::ExchangeDiagnostics(Allocator& allocator) noexcept
    : allocator_(&allocator), items_(allocator) {
}

Result<void> ExchangeDiagnostics::TryAdd(const ExchangeDiagnosticInput& input) noexcept {
    if (items_.Size() >= UnrealExchangeMaxDiagnostics) {
        ++dropped_;
        return Result<void>::Success();
    }

    ExchangeDiagnostic item(*allocator_);
    item.code = input.code;
    item.severity = ExchangeSeverityOf(input.code);
    item.error = input.error;
    item.issue = input.issue;
    item.file = input.file;
    item.asset = input.asset;
    item.related = input.related;
    item.position = input.position;
    if (auto assigned = item.text.TryAssign(input.text); !assigned) {
        return assigned;
    }

    return items_.TryPushBack(Move(item));
}

usize ExchangeDiagnostics::Count(ExchangeSeverity severity) const noexcept {
    usize count = 0;
    for (const ExchangeDiagnostic& item : items_) {
        count += item.severity == severity ? 1 : 0;
    }

    return count;
}

usize ExchangeDiagnostics::Count(AssetDiagnosticCode code) const noexcept {
    usize count = 0;
    for (const ExchangeDiagnostic& item : items_) {
        count += item.code == code ? 1 : 0;
    }

    return count;
}

const ExchangeDiagnostic* ExchangeDiagnostics::Find(AssetDiagnosticCode code) const noexcept {
    for (const ExchangeDiagnostic& item : items_) {
        if (item.code == code) {
            return &item;
        }
    }

    return nullptr;
}

void ExchangeDiagnostics::Clear() noexcept {
    items_.Clear();
    dropped_ = 0;
}

} // namespace rocks
