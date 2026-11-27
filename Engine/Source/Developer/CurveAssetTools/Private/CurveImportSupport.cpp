#include "CurveImportSupport.h"

#include <Core/Text/NumberText.h>
#include <Core/Text/Utf16.h>
#include <Platform/PlatformMath.h>

// 커브 임포터의 공용 도우미다(Docs/Design/CurveAssets 5.1.1절).
namespace rocks::curve_import_detail {

namespace {

constexpr uint8 Utf8Bom[] = {0xEF, 0xBB, 0xBF};
constexpr uint8 Utf16LittleEndianBom[] = {0xFF, 0xFE};
constexpr uint8 Utf16BigEndianBom[] = {0xFE, 0xFF};

bool StartsWith(Span<const uint8> bytes, Span<const uint8> prefix) noexcept {
    if (bytes.Size() < prefix.Size()) {
        return false;
    }

    for (usize index = 0; index < prefix.Size(); ++index) {
        if (bytes[index] != prefix[index]) {
            return false;
        }
    }

    return true;
}

bool IsSpace(char value) noexcept {
    return value == ' ' || value == '\t';
}

} // namespace

Result<void> DecodeImportText(Span<const uint8> bytes, String& out) noexcept {
    String staged(out.GetAllocator());
    if (StartsWith(bytes, Span<const uint8>(Utf16LittleEndianBom)) ||
        StartsWith(bytes, Span<const uint8>(Utf16BigEndianBom))) {
        const Utf16ByteOrder order = bytes[0] == Utf16LittleEndianBom[0]
                                         ? Utf16ByteOrder::LittleEndian
                                         : Utf16ByteOrder::BigEndian;
        auto converted = TryAppendUtf16AsUtf8(
            bytes.Slice(sizeof(Utf16LittleEndianBom), bytes.Size() - sizeof(Utf16LittleEndianBom)),
            order, staged);
        if (!converted) {
            return Result<void>::Failure(converted.Error() == ErrorCode::InvalidArgument
                                             ? ErrorCode::CorruptData
                                             : converted.Error());
        }
    } else {
        const usize skip = StartsWith(bytes, Span<const uint8>(Utf8Bom)) ? sizeof(Utf8Bom) : 0;
        const StringView text(reinterpret_cast<const char*>(bytes.Data()) + skip,
                              bytes.Size() - skip);
        if (!text.ValidateUtf8()) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (auto assigned = staged.TryAssign(text); !assigned) {
            return assigned;
        }
    }

    out = Move(staged);
    return Result<void>::Success();
}

Result<float> ParseNumberCell(StringView cell) noexcept {
    usize first = 0;
    usize last = cell.ByteCount();
    while (first < last && IsSpace(cell.Data()[first])) {
        ++first;
    }

    while (last > first && IsSpace(cell.Data()[last - 1])) {
        --last;
    }

    auto parsed = TryParseDouble(StringView(cell.Data() + first, last - first));
    if (!parsed) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    const float value = static_cast<float>(parsed.Value());
    if (!platform::PlatformMath::IsFinite(value)) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<float>::Success(value);
}

Result<float> ParseJsonNumberString(StringView text) noexcept {
    const char* data = text.Data();
    const usize count = text.ByteCount();
    if (count == 0) {
        return Result<float>::Failure(ErrorCode::InvalidArgument);
    }

    usize index = data[0] == '-' || data[0] == '+' ? 1 : 0;
    bool dot = false;
    bool digit = false;
    for (; index < count; ++index) {
        if (data[index] == '.') {
            if (dot) {
                return Result<float>::Failure(ErrorCode::InvalidArgument);
            }

            dot = true;
        } else if (data[index] >= '0' && data[index] <= '9') {
            digit = true;
        } else {
            return Result<float>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // UE Atod는 숫자가 없는 "."·"-"를 0으로 읽는다.
    if (!digit) {
        return Result<float>::Success(0.0f);
    }

    return ParseNumberCell(text);
}

Result<void> FailRowName(AssetImportContext& context, ErrorCode error, uint64 position) noexcept {
    if (error == ErrorCode::OutOfMemory || error == ErrorCode::CapacityExceeded) {
        return Result<void>::Failure(error);
    }

    return context.Fail(AssetDiagnosticCode::InvalidSource, error, position);
}

Result<Name> MakeRowName(NamePool& names, StringView raw, Allocator& allocator) noexcept {
    String fixed(allocator);
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        bool invalid = false;
        for (usize check = 0; check < CurveTableInvalidNameCharacters.ByteCount(); ++check) {
            invalid = invalid || raw.Data()[index] == CurveTableInvalidNameCharacters.Data()[check];
        }

        if (!invalid) {
            if (auto appended = fixed.TryAppend(StringView(raw.Data() + index, 1)); !appended) {
                return Result<Name>::Failure(appended.Error());
            }
        }
    }

    if (fixed.View().IsEmpty()) {
        return Result<Name>::Success(Name{});
    }

    return names.TryIntern(fixed.View());
}

Result<void> ReadSourceText(AssetImportContext& context, String& out) noexcept {
    const Span<const AssetImportSource> sources = context.Sources();
    if (sources.Size() != 1) {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                            0);
    }

    auto decoded = DecodeImportText(sources[0].bytes, out);
    if (!decoded) {
        if (decoded.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(decoded.Error());
        }

        return context.Fail(AssetDiagnosticCode::InvalidSource, decoded.Error(), 0);
    }

    return Result<void>::Success();
}

} // namespace rocks::curve_import_detail
