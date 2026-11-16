#include <Object/PathKey.h>
#include <Object/SoftObjectRef.h>
#include <Base/Endian.h>
#include <Core/Text/Name.h>

// 경로 key 단계의 키 wire 정규형(Object/PathKey.h)이다. 종류별 byte는 그래프 저장의 ValueArray
// 원소와 같다(Object/FieldValueWire.h WriteValueWire — Archive WriteBool·WriteU32·WriteU64·
// WriteI64·WriteGuid·WriteBytes·WriteString).
namespace rocks {

namespace {

constexpr usize GuidBytes = 16;
constexpr usize LengthBytes = sizeof(uint64);

// 좁은 부호 없는 정수 종류의 상한이다(UInt64는 검사하지 않는다).
uint64 UnsignedKeyLimit(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::UInt8:
        return NumericLimits<uint8>::Max();
    case ValueKind::UInt16:
        return NumericLimits<uint16>::Max();
    case ValueKind::UInt32:
        return NumericLimits<uint32>::Max();
    default:
        return NumericLimits<uint64>::Max();
    }
}

// 부호 있는 정수 종류의 범위 안인지다(Int64·Enum은 늘 참 — enum 선언 값은 경로 연산이 본다).
bool FitsSignedKey(ValueKind kind, int64 value) noexcept {
    switch (kind) {
    case ValueKind::Int8:
        return value >= NumericLimits<int8>::Min() && value <= NumericLimits<int8>::Max();
    case ValueKind::Int16:
        return value >= NumericLimits<int16>::Min() && value <= NumericLimits<int16>::Max();
    case ValueKind::Int32:
        return value >= NumericLimits<int32>::Min() && value <= NumericLimits<int32>::Max();
    default:
        return true;
    }
}

void StoreGuid(uint8* out, Guid value) noexcept {
    StoreLittleEndian<uint64>(out, value.high);
    StoreLittleEndian<uint64>(out + 8, value.low);
}

Guid LoadGuid(const uint8* bytes) noexcept {
    return Guid{LoadLittleEndian<uint64>(bytes), LoadLittleEndian<uint64>(bytes + 8)};
}

// 길이 prefix + 문자열 byte를 쓴다.
usize StoreText(uint8* out, StringView text) noexcept {
    StoreLittleEndian<uint64>(out, static_cast<uint64>(text.ByteCount()));
    for (usize index = 0; index < text.ByteCount(); ++index) {
        out[LengthBytes + index] = static_cast<uint8>(text.Data()[index]);
    }

    return LengthBytes + text.ByteCount();
}

// bytes[offset..]의 길이 prefix 문자열이 끝까지 정확히 차는지 읽는다(UTF-8 검사 포함).
bool LoadText(Span<const uint8> bytes, usize offset, StringView& text) noexcept {
    if (bytes.Size() < offset || bytes.Size() - offset < LengthBytes) {
        return false;
    }

    const uint64 length = LoadLittleEndian<uint64>(bytes.Data() + offset);
    if (length != bytes.Size() - offset - LengthBytes) {
        return false;
    }

    text = StringView(reinterpret_cast<const char*>(bytes.Data() + offset + LengthBytes),
                      static_cast<usize>(length));
    return text.ValidateUtf8().IsOk();
}

// 키 값의 wire 문자열이다: Name의 None(빈 문자열·"None"·빈 base 번호 이름 등 NameTextEquals가
// None으로 보는 원문)은 저장 wire와 같이 빈 문자열이다(Object/NameField.h).
StringView WireText(const PathKeyValue& value) noexcept {
    if (value.kind == ValueKind::Name && NameTextEquals(value.text, StringView())) {
        return StringView();
    }

    return value.text;
}

} // namespace

namespace detail {

Result<usize> PathKeyWireBytes(const PathKeyValue& value) noexcept {
    if (!ValueKindCanBeMapKey(value.kind)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    switch (value.kind) {
    case ValueKind::Bool:
        return Result<usize>::Success(1);
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        if (value.unsignedInteger > UnsignedKeyLimit(value.kind)) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        return Result<usize>::Success(sizeof(uint32));
    case ValueKind::UInt64:
        return Result<usize>::Success(sizeof(uint64));
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
        if (!FitsSignedKey(value.kind, value.signedInteger)) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        return Result<usize>::Success(sizeof(int64));
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        return Result<usize>::Success(GuidBytes);
    case ValueKind::Digest256:
        return Result<usize>::Success(LengthBytes + Sha256Digest::Bytes);
    case ValueKind::Utf8String:
    case ValueKind::Name:
        if (!value.text.ValidateUtf8()) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (value.text.ByteCount() > NumericLimits<usize>::Max() - LengthBytes) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }

        return Result<usize>::Success(LengthBytes + WireText(value).ByteCount());
    case ValueKind::SoftObjectReference:
        if (auto valid = SoftObjectRef<>::Validate(AssetId{value.guid}, value.text); !valid) {
            return Result<usize>::Failure(valid.Error());
        }

        return Result<usize>::Success(GuidBytes + LengthBytes + value.text.ByteCount());
    default:
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
}

Result<usize> WritePathKeyWire(const PathKeyValue& value, Span<uint8> out) noexcept {
    auto size = PathKeyWireBytes(value);
    if (!size) {
        return size;
    }

    if (out.Size() < size.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8* bytes = out.Data();
    switch (value.kind) {
    case ValueKind::Bool:
        bytes[0] = value.boolean ? 1 : 0;
        break;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        StoreLittleEndian<uint32>(bytes, static_cast<uint32>(value.unsignedInteger));
        break;
    case ValueKind::UInt64:
        StoreLittleEndian<uint64>(bytes, value.unsignedInteger);
        break;
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        StoreGuid(bytes, value.guid);
        break;
    case ValueKind::Digest256:
        StoreLittleEndian<uint64>(bytes, uint64{Sha256Digest::Bytes});
        for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
            bytes[LengthBytes + index] = value.digest.bytes[index];
        }

        break;
    case ValueKind::Utf8String:
    case ValueKind::Name:
        StoreText(bytes, WireText(value));
        break;
    case ValueKind::SoftObjectReference:
        StoreGuid(bytes, value.guid);
        StoreText(bytes + GuidBytes, value.text);
        break;
    default:
        // 부호 있는 정수·enum(PathKeyWireBytes가 그 밖의 종류를 거부했다).
        StoreLittleEndian<uint64>(bytes, static_cast<uint64>(value.signedInteger));
        break;
    }

    return size;
}

} // namespace detail

Result<void> AppendPathKey(const PathKeyValue& value, Array<uint8>& out) noexcept {
    auto size = detail::PathKeyWireBytes(value);
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    const usize start = out.Size();
    if (size.Value() > NumericLimits<usize>::Max() - start) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto grown = out.TryResize(start + size.Value()); !grown) {
        return grown;
    }

    // 크기를 이미 확인했으므로 쓰기는 실패하지 않는다.
    auto written =
        detail::WritePathKeyWire(value, Span<uint8>(out.Data() + start, out.Size() - start));
    if (!written) {
        (void)out.TryResize(start);
        return Result<void>::Failure(written.Error());
    }

    return Result<void>::Success();
}

Result<PathKeyValue> ReadPathKey(const PathKey& key) noexcept {
    const auto invalid = []() noexcept {
        return Result<PathKeyValue>::Failure(ErrorCode::InvalidArgument);
    };

    if (!ValueKindCanBeMapKey(key.kind)) {
        return invalid();
    }

    const Span<const uint8> bytes = key.wire;
    PathKeyValue value;
    value.kind = key.kind;
    switch (key.kind) {
    case ValueKind::Bool:
        if (bytes.Size() != 1 || bytes[0] > 1) {
            return invalid();
        }

        value.boolean = bytes[0] == 1;
        break;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        if (bytes.Size() != sizeof(uint32)) {
            return invalid();
        }

        value.unsignedInteger = LoadLittleEndian<uint32>(bytes.Data());
        if (value.unsignedInteger > UnsignedKeyLimit(key.kind)) {
            return invalid();
        }

        break;
    case ValueKind::UInt64:
        if (bytes.Size() != sizeof(uint64)) {
            return invalid();
        }

        value.unsignedInteger = LoadLittleEndian<uint64>(bytes.Data());
        break;
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
        if (bytes.Size() != sizeof(int64)) {
            return invalid();
        }

        value.signedInteger = static_cast<int64>(LoadLittleEndian<uint64>(bytes.Data()));
        if (!FitsSignedKey(key.kind, value.signedInteger)) {
            return invalid();
        }

        break;
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        if (bytes.Size() != GuidBytes) {
            return invalid();
        }

        value.guid = LoadGuid(bytes.Data());
        break;
    case ValueKind::Digest256:
        if (bytes.Size() != LengthBytes + Sha256Digest::Bytes ||
            LoadLittleEndian<uint64>(bytes.Data()) != Sha256Digest::Bytes) {
            return invalid();
        }

        for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
            value.digest.bytes[index] = bytes[LengthBytes + index];
        }

        break;
    case ValueKind::Utf8String:
        if (!LoadText(bytes, 0, value.text)) {
            return invalid();
        }

        break;
    case ValueKind::Name:
        // None은 빈 문자열 하나로만 쓴다(저장 wire와 같은 정규형).
        if (!LoadText(bytes, 0, value.text) ||
            (value.text.ByteCount() != 0 && NameTextEquals(value.text, StringView()))) {
            return invalid();
        }

        break;
    case ValueKind::SoftObjectReference:
        if (bytes.Size() < GuidBytes || !LoadText(bytes, GuidBytes, value.text)) {
            return invalid();
        }

        value.guid = LoadGuid(bytes.Data());
        if (!SoftObjectRef<>::Validate(AssetId{value.guid}, value.text)) {
            return invalid();
        }

        break;
    default:
        return invalid();
    }

    return Result<PathKeyValue>::Success(value);
}

} // namespace rocks
