#include <SparksCompiler/Source.h>

#include <Base/NumericLimits.h>
#include <Core/Move.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Diagnostic.h>

#include "TextColumns.h"

namespace rocks::sparks {

namespace {

constexpr char LineFeed = '\n';
constexpr char CarriageReturn = '\r';

// UTF-8로 쓴 U+FEFF(BOM)다.
constexpr uint8 Utf8Bom[Utf8BomBytes] = {0xEFu, 0xBBu, 0xBFu};

constexpr bool StartsWithUtf8Bom(StringView text) noexcept {
    if (text.ByteCount() < Utf8BomBytes) {
        return false;
    }

    for (usize index = 0; index < Utf8BomBytes; ++index) {
        if (static_cast<uint8>(text.Data()[index]) != Utf8Bom[index]) {
            return false;
        }
    }

    return true;
}

// LF가 뒤따르지 않는 첫 CR의 위치다. 없으면 IndexNone이다.
usize FindLoneCarriageReturn(StringView text, usize from) noexcept {
    for (usize index = from; index < text.ByteCount(); ++index) {
        if (text.Data()[index] != CarriageReturn) {
            continue;
        }

        const bool followedByLineFeed =
            index + 1 < text.ByteCount() && text.Data()[index + 1] == LineFeed;
        if (!followedByLineFeed) {
            return index;
        }
    }

    return IndexNone;
}

// 원문 검사 진단의 문장이다(영어 — 실행 중 메시지 규칙).
constexpr StringView InvalidUtf8Message = "Source is not valid UTF-8.";
constexpr StringView InvalidUtf8Fix = "Save the file with UTF-8 encoding.";
constexpr StringView LoneCarriageReturnMessage = "Carriage return without a line feed.";
constexpr StringView LoneCarriageReturnExplanation =
    "Some editors treat a lone CR as a line break and others do not, so line numbers would "
    "disagree.";
constexpr StringView LoneCarriageReturnFix = "Use LF or CRLF line endings.";
constexpr StringView SourceTooLargeMessage = "Source file is larger than 16 MiB.";
constexpr StringView SourceTooLargeFix = "Split the file into smaller modules.";

static_assert(MaxSourceBytes == usize{16} * 1024 * 1024,
              "SourceTooLargeMessage states the limit in MiB");

// 진단 설명 문장을 담을 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize ExplanationInlineBytes = 128;

} // namespace

SourceFile::SourceFile(Allocator& allocator) noexcept
    : path_(allocator), text_(allocator), lineStarts_(allocator) {
}

SourceLocation SourceFile::Locate(uint32 offset) const noexcept {
    if (offset > text_.Size()) {
        platform::Fatal("Sparks source offset is out of range");
    }

    // offset보다 크지 않은 마지막 줄 시작을 이분 탐색으로 찾는다. 첫 줄 시작은 ContentBegin이므로
    // BOM 안의 위치는 1줄 1열로 본다.
    usize low = 0;
    usize high = lineStarts_.Size();
    while (high - low > 1) {
        const usize middle = low + (high - low) / 2;
        if (lineStarts_[middle] <= offset) {
            low = middle;
        } else {
            high = middle;
        }
    }

    const uint32 lineBegin = lineStarts_[low];
    SourceLocation location;
    location.line = static_cast<uint32>(low) + 1;
    location.column =
        offset <= lineBegin
            ? 1
            : detail::CountColumns(text_.Data() + lineBegin, text_.Data() + offset) + 1;
    return location;
}

uint32 SourceFile::LineBegin(uint32 line) const noexcept {
    if (line == 0 || line > lineStarts_.Size()) {
        platform::Fatal("Sparks source line is out of range");
    }

    return lineStarts_[line - 1];
}

StringView SourceFile::LineText(uint32 line) const noexcept {
    const uint32 begin = LineBegin(line);
    usize end = line < lineStarts_.Size() ? lineStarts_[line] - 1 : text_.Size();
    // 다음 줄 시작 바로 앞은 LF다. 그 앞의 CR도 줄 끝의 일부다.
    if (line < lineStarts_.Size() && end > begin && text_[end - 1] == CarriageReturn) {
        --end;
    }

    return StringView(text_.Data() + begin, end - begin);
}

SourceSet::SourceSet(Allocator& allocator) noexcept : allocator_(&allocator), files_(allocator) {
}

const SourceFile& SourceSet::Get(SourceId id) const noexcept {
    if (id.index >= files_.Size()) {
        platform::Fatal("Sparks source id is out of range");
    }

    return files_[id.index];
}

Result<SourceId> SourceSet::TryAdd(StringView path, StringView text,
                                   DiagnosticList& diagnostics) noexcept {
    if (files_.Size() >= NumericLimits<uint32>::Max()) {
        return Result<SourceId>::Failure(ErrorCode::CapacityExceeded);
    }

    const SourceId id{static_cast<uint32>(files_.Size())};
    SourceFile file(*allocator_);
    if (auto assigned = file.path_.TryAssign(path); !assigned) {
        return Result<SourceId>::Failure(assigned.Error());
    }

    // 진단은 먼저 만들어 두고, 용량을 확보한 뒤 파일과 함께 한 번에 넣는다(실패하면 둘 다 그대로).
    Array<Diagnostic> prepared(*allocator_);
    if (auto reserved = prepared.TryReserve(1); !reserved) {
        return Result<SourceId>::Failure(reserved.Error());
    }

    if (text.ByteCount() > MaxSourceBytes) {
        // 원문을 복사하지 않는다. 줄 표는 빈 원문의 한 줄이다.
        if (auto pushed = file.lineStarts_.TryPushBack(0); !pushed) {
            return Result<SourceId>::Failure(pushed.Error());
        }

        StringBuilder<ExplanationInlineBytes> explanation(*allocator_);
        if (auto formatted = explanation.TryAppendFormat(
                "The file has {} bytes; the limit is {} bytes.", text.ByteCount(), MaxSourceBytes);
            !formatted) {
            return Result<SourceId>::Failure(formatted.Error());
        }

        auto diagnostic =
            diagnostics.TryPrepare(DiagnosticCode::SourceTooLarge, id, false, SourceSpan{},
                                   SourceTooLargeMessage, explanation.View(), SourceTooLargeFix);
        if (!diagnostic) {
            return Result<SourceId>::Failure(diagnostic.Error());
        }

        if (!prepared.TryPushBack(Move(diagnostic.Value()))) {
            platform::Fatal("Sparks reserved diagnostic push failed");
        }
    } else {
        if (auto resized = file.text_.TryResize(text.ByteCount()); !resized) {
            return Result<SourceId>::Failure(resized.Error());
        }

        for (usize index = 0; index < text.ByteCount(); ++index) {
            file.text_[index] = text.Data()[index];
        }

        file.contentBegin_ = StartsWithUtf8Bom(text) ? static_cast<uint32>(Utf8BomBytes) : 0;
        if (auto pushed = file.lineStarts_.TryPushBack(file.contentBegin_); !pushed) {
            return Result<SourceId>::Failure(pushed.Error());
        }

        for (usize index = file.contentBegin_; index < text.ByteCount(); ++index) {
            if (text.Data()[index] != LineFeed) {
                continue;
            }

            if (auto pushed = file.lineStarts_.TryPushBack(static_cast<uint32>(index + 1));
                !pushed) {
                return Result<SourceId>::Failure(pushed.Error());
            }
        }

        file.usable_ = true;
        const usize invalid = text.FindInvalidUtf8();
        if (invalid != IndexNone) {
            file.usable_ = false;
            StringBuilder<ExplanationInlineBytes> explanation(*allocator_);
            if (auto formatted = explanation.TryAppendFormat(
                    "The byte 0x{:02X} at this position does not start a valid UTF-8 sequence.",
                    static_cast<uint8>(text.Data()[invalid]));
                !formatted) {
                return Result<SourceId>::Failure(formatted.Error());
            }

            const SourceSpan span{static_cast<uint32>(invalid), static_cast<uint32>(invalid + 1)};
            auto diagnostic =
                diagnostics.TryPrepare(DiagnosticCode::InvalidUtf8, id, true, span,
                                       InvalidUtf8Message, explanation.View(), InvalidUtf8Fix);
            if (!diagnostic) {
                return Result<SourceId>::Failure(diagnostic.Error());
            }

            if (!prepared.TryPushBack(Move(diagnostic.Value()))) {
                platform::Fatal("Sparks reserved diagnostic push failed");
            }
        } else {
            const usize loneCr = FindLoneCarriageReturn(text, file.contentBegin_);
            if (loneCr != IndexNone) {
                const SourceSpan span{static_cast<uint32>(loneCr), static_cast<uint32>(loneCr + 1)};
                auto diagnostic = diagnostics.TryPrepare(
                    DiagnosticCode::LoneCarriageReturn, id, true, span, LoneCarriageReturnMessage,
                    LoneCarriageReturnExplanation, LoneCarriageReturnFix);
                if (!diagnostic) {
                    return Result<SourceId>::Failure(diagnostic.Error());
                }

                if (!prepared.TryPushBack(Move(diagnostic.Value()))) {
                    platform::Fatal("Sparks reserved diagnostic push failed");
                }
            }
        }
    }

    if (auto reserved = files_.TryEnsureCapacity(files_.Size() + 1); !reserved) {
        return Result<SourceId>::Failure(reserved.Error());
    }

    if (auto reserved = diagnostics.TryReserveMore(prepared.Size()); !reserved) {
        return Result<SourceId>::Failure(reserved.Error());
    }

    // 여기부터는 실패하지 않는다.
    if (!files_.TryPushBack(Move(file))) {
        platform::Fatal("Sparks reserved source push failed");
    }

    for (usize index = 0; index < prepared.Size(); ++index) {
        diagnostics.Commit(Move(prepared[index]));
    }

    return Result<SourceId>::Success(id);
}

} // namespace rocks::sparks
