#include <Serialization/ValueArray.h>
#include <Base/Endian.h>

namespace rocks {
namespace {

constexpr usize HeaderBytes = 8; // u32 원소 tag + u32 count

// count개의 원소가 차지하는 최소 byte다. usize를 넘으면 false다.
bool ElementBytes(uint32 count, usize elementBytes, usize& total) noexcept {
    if (elementBytes != 0 && count > static_cast<usize>(-1) / elementBytes) {
        return false;
    }

    total = static_cast<usize>(count) * elementBytes;
    return true;
}

// 원소 wire와 스키마 shape를 합친 크기다(reader·writer 공통). 원소 tag가 아니거나 모순 조합(고정 폭
// wire에 다른 폭, exact인데 wire 최소보다 작은 크기)이면 InvalidArgument다.
Result<ValueArrayShape> ResolveShape(uint32 elementWire, ValueArrayShape shape) noexcept {
    auto wire = ValueArrayElementShape(elementWire);
    if (!wire) {
        return wire;
    }

    const ValueArrayShape element = wire.Value();
    if ((element.exact && shape.minElementBytes != 0 &&
         shape.minElementBytes != element.minElementBytes) ||
        (shape.exact && shape.minElementBytes < element.minElementBytes)) {
        return Result<ValueArrayShape>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<ValueArrayShape>::Success({shape.minElementBytes > element.minElementBytes
                                                 ? shape.minElementBytes
                                                 : element.minElementBytes,
                                             element.exact || shape.exact});
}

// 가변 wire 원소의 길이 prefix 위치와 고정 부분 크기다(String·Bytes는 0·8, SoftObjectReference는
// Guid 뒤 16·24). 고정 폭 wire면 false다.
bool LengthPrefix(uint32 elementWire, usize& offset, usize& fixedBytes) noexcept {
    switch (static_cast<WireType>(elementWire)) {
    case WireType::String:
    case WireType::Bytes:
        offset = 0;
        fixedBytes = 8;
        return true;
    case WireType::SoftObjectReference:
        offset = 16;
        fixedBytes = 24;
        return true;
    default:
        return false;
    }
}

} // namespace

Result<ValueArrayShape> ValueArrayElementShape(uint32 elementWire) noexcept {
    switch (static_cast<WireType>(elementWire)) {
    case WireType::Bool:
        return Result<ValueArrayShape>::Success({1, true});
    case WireType::U32:
    case WireType::F32:
        return Result<ValueArrayShape>::Success({4, true});
    case WireType::U64:
    case WireType::I64:
    case WireType::F64:
        return Result<ValueArrayShape>::Success({8, true});
    case WireType::Guid:
        return Result<ValueArrayShape>::Success({16, true});
    case WireType::String:
    case WireType::Bytes:
        return Result<ValueArrayShape>::Success({8, false});
    case WireType::SoftObjectReference:
        return Result<ValueArrayShape>::Success({24, false});
    case WireType::Record:
    case WireType::RecordList:
    case WireType::ValueArray:
        break;
    }

    return Result<ValueArrayShape>::Failure(ErrorCode::InvalidArgument);
}

ValueArrayReader::ValueArrayReader(ArchiveReader&& archive, uint32 count) noexcept
    : archive_(Move(archive)), count_(count) {
}

ValueArrayReader::ValueArrayReader(ValueArrayReader&& other) noexcept
    : archive_(Move(other.archive_)), count_(other.count_) {
    // 이동된 원본은 원소 0개의 빈 reader다(ArchiveReader 원본도 빈 입력이 된다).
    other.count_ = 0;
}

ValueArrayReader& ValueArrayReader::operator=(ValueArrayReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        count_ = other.count_;
        other.count_ = 0;
    }

    return *this;
}

Result<ValueArrayReader> ValueArrayReader::Open(ArchiveReader&& payload, uint32 elementWire,
                                                uint32 maxCount, ValueArrayShape shape) noexcept {
    auto resolved = ResolveShape(elementWire, shape);
    if (!resolved) {
        return Result<ValueArrayReader>::Failure(resolved.Error());
    }

    ArchiveReader archive = Move(payload);
    auto stored = archive.ReadU32();
    if (!stored) {
        return Result<ValueArrayReader>::Failure(stored.Error());
    }

    // 원소가 될 수 없는 tag(중첩 배열·Record·모르는 값)는 형식 오류이고, 원소 tag끼리 다르면 형식
    // 불일치다.
    if (!ValueArrayElementShape(stored.Value())) {
        return Result<ValueArrayReader>::Failure(ErrorCode::InvalidArgument);
    }

    if (stored.Value() != elementWire) {
        return Result<ValueArrayReader>::Failure(ErrorCode::TypeMismatch);
    }

    auto count = archive.ReadU32();
    if (!count) {
        return Result<ValueArrayReader>::Failure(count.Error());
    }

    if (count.Value() > maxCount) {
        return Result<ValueArrayReader>::Failure(ErrorCode::CapacityExceeded);
    }

    // 외부 count만 믿고 원소를 읽거나 호출자가 할당하지 않게, 남은 byte가 원소 최소 크기를 담는지
    // 먼저 본다(고정 폭·exact는 정확히 같아야 한다).
    const usize elementBytes = resolved.Value().minElementBytes;
    const bool exact = resolved.Value().exact;
    usize required = 0;
    if (!ElementBytes(count.Value(), elementBytes, required) || required > archive.Remaining() ||
        (exact && required != archive.Remaining())) {
        return Result<ValueArrayReader>::Failure(ErrorCode::InvalidArgument);
    }

    // count만큼의 work를 원소 검사·읽기 전에 청구한다(아래 exact 검사의 원소당 비교도 덮는다).
    auto spent = archive.Spend(count.Value());
    if (!spent) {
        return Result<ValueArrayReader>::Failure(spent.Error());
    }

    // 가변 wire의 exact: 합계만 맞고 원소 경계가 어긋난 입력(예: Digest 길이 0과 64)을 거부한다.
    // 남은 byte가 정확히 count × 크기이므로 원소 i의 길이 prefix는 i × 크기 + offset에 있다.
    usize offset = 0;
    usize fixedBytes = 0;
    if (exact && LengthPrefix(elementWire, offset, fixedBytes)) {
        const uint8* elements = archive.source_.Data() + archive.position_;
        const auto expected = static_cast<uint64>(elementBytes - fixedBytes);
        for (uint32 index = 0; index < count.Value(); ++index) {
            const usize at = static_cast<usize>(index) * elementBytes + offset;
            if (LoadLittleEndian<uint64>(elements + at) != expected) {
                return Result<ValueArrayReader>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<ValueArrayReader>::Success(ValueArrayReader(Move(archive), count.Value()));
}

Result<ValueArrayReader> ValueArrayReader::OpenField(const RecordReader& parent,
                                                     const FieldView& field, uint32 elementWire,
                                                     uint32 maxCount,
                                                     ValueArrayShape shape) noexcept {
    if (!parent.archive_.shared_) {
        return Result<ValueArrayReader>::Failure(ErrorCode::InvalidArgument); // 이동된 parent
    }

    // 여는 호출도 operation을 소비한다(부모 트리의 예산).
    if (auto charged = parent.archive_.shared_->Charge(1); !charged) {
        return Result<ValueArrayReader>::Failure(charged.Error());
    }

    if (field.wireType != static_cast<uint32>(WireType::ValueArray)) {
        return Result<ValueArrayReader>::Failure(ErrorCode::TypeMismatch);
    }

    if (!parent.Contains(field.payload)) {
        return Result<ValueArrayReader>::Failure(ErrorCode::InvalidArgument);
    }

    auto archive =
        ArchiveReader::CreateShared(field.payload, parent.limits_.archive, parent.archive_.shared_);
    if (!archive) {
        return Result<ValueArrayReader>::Failure(archive.Error());
    }

    return Open(Move(archive.Value()), elementWire, maxCount, shape);
}

uint32 ValueArrayReader::Count() const noexcept {
    return count_;
}

ArchiveReader& ValueArrayReader::Elements() noexcept {
    return archive_;
}

Result<void> ValueArrayReader::Finish() const noexcept {
    if (!archive_.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<ValueArrayWriter> ValueArrayWriter::Begin(ArchiveWriter& out, uint32 elementWire,
                                                 uint32 count, usize maxBytes, uint32 maxCount,
                                                 ValueArrayShape shape) noexcept {
    auto resolved = ResolveShape(elementWire, shape);
    if (!resolved) {
        return Result<ValueArrayWriter>::Failure(resolved.Error());
    }

    usize elements = 0;
    if (count > maxCount || maxBytes < HeaderBytes ||
        !ElementBytes(count, resolved.Value().minElementBytes, elements) ||
        elements > maxBytes - HeaderBytes) {
        return Result<ValueArrayWriter>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize start = out.ByteCount();
    if (auto written = out.WriteU32(elementWire); !written) {
        return Result<ValueArrayWriter>::Failure(written.Error());
    }

    if (auto written = out.WriteU32(count); !written) {
        return Result<ValueArrayWriter>::Failure(written.Error());
    }

    return Result<ValueArrayWriter>::Success(
        ValueArrayWriter(out, start, count, maxBytes, resolved.Value()));
}

Result<void> ValueArrayWriter::Check() const noexcept {
    if (out_->ByteCount() - start_ > maxBytes_) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

Result<void> ValueArrayWriter::End() const noexcept {
    const usize total = out_->ByteCount() - start_;
    if (total > maxBytes_) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // Begin이 maxBytes 안임을 확인했으므로 넘치지 않는다. 가변 원소는 합계의 하한만 본다.
    const usize minimum = HeaderBytes + static_cast<usize>(count_) * shape_.minElementBytes;
    if (total < minimum || (shape_.exact && total != minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks
