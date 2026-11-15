#include <Serialization/TextWire.h>
#include <Base/Endian.h>

#include "WireInternal.h"

namespace rocks {
namespace {

// 형태에 맞는 칸만 채운 값인지 본다(쓰기 전 검사).
bool IsConsistent(const TextWireValue& value) noexcept {
    const bool localizedFields = value.textId != Guid{} || value.sourceRevision != 0;
    switch (value.form) {
    case TextWireForm::Empty:
        return value.source.ByteCount() == 0 && !localizedFields;
    case TextWireForm::Invariant:
        return !localizedFields;
    case TextWireForm::Localized:
        return value.textId != Guid{} && value.sourceRevision != 0;
    }

    return false;
}

// Localized 내용(Guid + U64 + String)을 읽는다. 실패한 칸의 오류를 돌려준다(cursor 복원은 호출자).
Result<TextWireValue> ReadLocalized(ArchiveReader& in) noexcept {
    TextWireValue value;
    value.form = TextWireForm::Localized;
    auto id = in.ReadGuid();
    if (!id) {
        return Result<TextWireValue>::Failure(id.Error());
    }

    if (id.Value() == Guid{}) {
        return Result<TextWireValue>::Failure(ErrorCode::InvalidArgument);
    }

    auto revision = in.ReadU64();
    if (!revision) {
        return Result<TextWireValue>::Failure(revision.Error());
    }

    if (revision.Value() == 0) {
        return Result<TextWireValue>::Failure(ErrorCode::InvalidArgument);
    }

    auto source = in.ReadStringView();
    if (!source) {
        return Result<TextWireValue>::Failure(source.Error());
    }

    value.textId = id.Value();
    value.sourceRevision = revision.Value();
    value.source = source.Value();
    return Result<TextWireValue>::Success(value);
}

} // namespace

Result<void> WriteTextWire(ArchiveWriter& out, const TextWireValue& value) noexcept {
    if (!IsConsistent(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 고정 칸(형태 + Localized의 Guid·U64 + 원문 길이)을 prefix 하나로 모아 원문과 함께 한 번에
    // 붙인다. 원문이 out 자신의 View를 가리켜도 Append가 성장 뒤 위치를 보정하고, 실패하면 out은
    // 호출 전 byte 그대로다(Write*와 같은 규약).
    uint8 prefix[1 + 16 + 8 + detail::LengthPrefixBytes];
    usize used = 0;
    prefix[used++] = static_cast<uint8>(value.form);
    if (value.form == TextWireForm::Localized) {
        StoreLittleEndian<uint64>(prefix + used, value.textId.high);
        StoreLittleEndian<uint64>(prefix + used + 8, value.textId.low);
        StoreLittleEndian<uint64>(prefix + used + 16, value.sourceRevision);
        used += 24;
    }

    Span<const uint8> source;
    if (value.form != TextWireForm::Empty) {
        StoreLittleEndian<uint64>(prefix + used, static_cast<uint64>(value.source.ByteCount()));
        used += detail::LengthPrefixBytes;
        source = Span<const uint8>(reinterpret_cast<const uint8*>(value.source.Data()),
                                   value.source.ByteCount());
    }

    return detail::WireAccess::Append(out, Span<const uint8>(prefix, used), source,
                                      value.form != TextWireForm::Empty);
}

Result<TextWireValue> ReadTextWire(ArchiveReader& in) noexcept {
    const usize start = in.Position();
    auto form = in.ReadU8();
    if (!form) {
        return Result<TextWireValue>::Failure(form.Error());
    }

    Result<TextWireValue> value = Result<TextWireValue>::Failure(ErrorCode::InvalidArgument);
    switch (form.Value()) {
    case static_cast<uint8>(TextWireForm::Empty):
        return Result<TextWireValue>::Success(TextWireValue{});
    case static_cast<uint8>(TextWireForm::Invariant): {
        auto source = in.ReadStringView();
        if (source) {
            TextWireValue invariant;
            invariant.form = TextWireForm::Invariant;
            invariant.source = source.Value();
            return Result<TextWireValue>::Success(invariant);
        }

        value = Result<TextWireValue>::Failure(source.Error());
        break;
    }
    case static_cast<uint8>(TextWireForm::Localized):
        value = ReadLocalized(in);
        if (value) {
            return value;
        }

        break;
    default:
        break;
    }

    detail::WireAccess::Rewind(in, start);
    return value;
}

} // namespace rocks
