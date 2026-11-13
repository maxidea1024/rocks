#include <Serialization/Archive.h>
#include <Base/Endian.h>
#include <float.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

static_assert(sizeof(float) == 4 && sizeof(double) == 8);
static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);
static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);
static_assert(sizeof(usize) <= sizeof(uint64));

// 실제 주소 범위의 유효성은 caller 전제다. 자기 buffer를 가리킬 때만 재할당 후 주소를 보정한다.
bool IsSelfRange(Span<const uint8> storage, Span<const uint8> input, usize& offset) noexcept {
    if (storage.Size() == 0 || input.Size() == 0) {
        return false;
    }

    const uptr base = reinterpret_cast<uptr>(storage.Data());
    const uptr address = reinterpret_cast<uptr>(input.Data());
    if (address >= base && address - base < storage.Size()) {
        offset = address - base;
        return true;
    }

    return false;
}

} // namespace

ArchiveWriter::ArchiveWriter(Allocator& allocator, ArchiveLimits limits) noexcept
    : bytes_(allocator), limits_(limits) {
}

Result<void> ArchiveWriter::Charge(uint64 work) noexcept {
    if (operations_ == limits_.maxOperations) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    ++operations_;
    if (work > limits_.maxWork - work_) {
        work_ = limits_.maxWork;
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    work_ += work;
    return Result<void>::Success();
}

Result<void> ArchiveWriter::Append(Span<const uint8> prefix, Span<const uint8> payload,
                                   bool utf8) noexcept {
    usize required = 0;
    usize work = 0;
    if (!TryAdd(prefix.Size(), payload.Size(), required) ||
        !TryAdd(required, bytes_.Size(), required) ||
        !TryAdd(prefix.Size(), payload.Size(), work) || !TryAdd(work, 1, work) ||
        (utf8 && !TryAdd(work, payload.Size(), work)) ||
        (required > bytes_.Capacity() && !TryAdd(work, bytes_.Size(), work))) {
        auto charged = Charge(1);
        return Result<void>::Failure(charged ? ErrorCode::Overflow : charged.Error());
    }

    auto charged = Charge(static_cast<uint64>(work));
    if (!charged) {
        return charged;
    }

    if (required > limits_.maxTotalBytes || payload.Size() > limits_.maxValueBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (utf8) {
        auto valid = StringView(reinterpret_cast<const char*>(payload.Data()), payload.Size())
                         .ValidateUtf8();
        if (!valid) {
            return valid;
        }
    }

    return AppendReserved(prefix, payload);
}

Result<void> ArchiveWriter::Patch(usize offset, Span<const uint8> bytes) noexcept {
    auto charged = Charge(static_cast<uint64>(bytes.Size()));
    if (!charged) {
        return charged;
    }

    if (offset > bytes_.Size() || bytes.Size() > bytes_.Size() - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < bytes.Size(); ++index) {
        bytes_[offset + index] = bytes[index];
    }

    return Result<void>::Success();
}

Result<void> ArchiveWriter::AppendReserved(Span<const uint8> prefix,
                                           Span<const uint8> payload) noexcept {
    usize required = 0;
    if (!TryAdd(prefix.Size(), payload.Size(), required) ||
        !TryAdd(required, bytes_.Size(), required)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (required > limits_.maxTotalBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    usize offset = 0;
    const bool aliases = IsSelfRange(View(), payload, offset);
    if (aliases && payload.Size() > bytes_.Size() - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (required > bytes_.Capacity()) {
        const usize capacity = bytes_.Capacity();
        const usize growth = capacity / 2 + 1;
        usize preferred =
            growth > limits_.maxTotalBytes - capacity ? limits_.maxTotalBytes : capacity + growth;
        if (preferred < required) {
            preferred = required;
        }

        auto reserved = bytes_.TryReserve(preferred);
        if (!reserved) {
            return reserved;
        }
    }

    // 모든 실패 가능 작업을 먼저 끝낸다. 이후에는 예약된 uint8 슬롯만 생성하므로 부분 commit이
    // 없다.
    const uint8* data = aliases ? bytes_.Data() + offset : payload.Data();
    for (usize index = 0; index < prefix.Size(); ++index) {
        if (!bytes_.TryPushBack(prefix[index])) {
            platform::Fatal("Archive prefix reservation invariant failed");
        }
    }

    for (usize index = 0; index < payload.Size(); ++index) {
        if (!bytes_.TryPushBack(data[index])) {
            platform::Fatal("Archive payload reservation invariant failed");
        }
    }

    return Result<void>::Success();
}

Result<void> ArchiveWriter::WriteBool(bool value) noexcept {
    const uint8 bytes[] = {static_cast<uint8>(value ? 1 : 0)};
    return Append(bytes, {});
}

Result<void> ArchiveWriter::WriteU32(uint32 value) noexcept {
    uint8 bytes[4];
    StoreLittleEndian<uint32>(bytes, value);
    return Append(bytes, {});
}

Result<void> ArchiveWriter::WriteU64(uint64 value) noexcept {
    uint8 bytes[8];
    StoreLittleEndian<uint64>(bytes, value);
    return Append(bytes, {});
}

Result<void> ArchiveWriter::WriteI64(int64 value) noexcept {
    return WriteU64(static_cast<uint64>(value));
}

Result<void> ArchiveWriter::WriteF32(float value) noexcept {
    return WriteU32(platform::BitCast<uint32>(value));
}

Result<void> ArchiveWriter::WriteF64(double value) noexcept {
    return WriteU64(platform::BitCast<uint64>(value));
}

Result<void> ArchiveWriter::WriteGuid(Guid value) noexcept {
    uint8 bytes[16];
    StoreLittleEndian<uint64>(bytes, value.high);
    StoreLittleEndian<uint64>(bytes + 8, value.low);
    return Append(bytes, {});
}

Result<void> ArchiveWriter::WriteBytes(Span<const uint8> value) noexcept {
    uint8 prefix[8];
    StoreLittleEndian<uint64>(prefix, static_cast<uint64>(value.Size()));
    return Append(prefix, value);
}

Result<void> ArchiveWriter::WriteString(StringView value) noexcept {
    uint8 prefix[8];
    StoreLittleEndian<uint64>(prefix, static_cast<uint64>(value.ByteCount()));
    return Append(prefix, {reinterpret_cast<const uint8*>(value.Data()), value.ByteCount()}, true);
}

Result<void> ArchiveWriter::WriteRaw(Span<const uint8> value) noexcept {
    return Append({}, value);
}

Span<const uint8> ArchiveWriter::View() const noexcept {
    return bytes_.View();
}

usize ArchiveWriter::ByteCount() const noexcept {
    return bytes_.Size();
}

uint64 ArchiveWriter::OperationsUsed() const noexcept {
    return operations_;
}

uint64 ArchiveWriter::WorkUsed() const noexcept {
    return work_;
}

Array<uint8> ArchiveWriter::TakeBytes() noexcept {
    return Move(bytes_);
}

Result<void> ArchiveBudget::Spend(uint64 work) noexcept {
    // 원자적 누적: 한도를 넘으면 사용량을 한도로 포화한다(줄이지 않는다).
    uint64 current = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&work_);
    while (true) {
        if (work > maxWork_ - current) {
            while (current < maxWork_ &&
                   !platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Relaxed,
                                                        platform::MemoryOrder::Relaxed>(
                       &work_, current, maxWork_)) {
            }

            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        if (platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Relaxed,
                                                platform::MemoryOrder::Relaxed>(&work_, current,
                                                                                current + work)) {
            return Result<void>::Success();
        }
    }
}

Result<void> ArchiveBudget::Charge(uint64 work) noexcept {
    uint64 current = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&operations_);
    while (true) {
        if (current >= maxOperations_) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        if (platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Relaxed,
                                                platform::MemoryOrder::Relaxed>(
                &operations_, current, current + 1)) {
            break;
        }
    }

    return Spend(work);
}

uint64 ArchiveBudget::OperationsUsed() const noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&operations_);
}

uint64 ArchiveBudget::WorkUsed() const noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&work_);
}

ArchiveReader::ArchiveReader(Span<const uint8> source, ArchiveLimits limits,
                             SharedPtr<ArchiveBudget> shared) noexcept
    : source_(source), limits_(limits), shared_(Move(shared)) {
}

ArchiveReader::ArchiveReader(ArchiveReader&& other) noexcept
    : source_(other.source_), limits_(other.limits_), position_(other.position_),
      operations_(other.operations_), work_(other.work_), shared_(Move(other.shared_)) {
    // 이동된 원본은 빈 입력이다(남은 자기 예산으로 같은 입력을 다시 읽지 못한다).
    other.source_ = Span<const uint8>();
    other.position_ = 0;
}

ArchiveReader& ArchiveReader::operator=(ArchiveReader&& other) noexcept {
    if (this != &other) {
        source_ = other.source_;
        limits_ = other.limits_;
        position_ = other.position_;
        operations_ = other.operations_;
        work_ = other.work_;
        shared_ = Move(other.shared_);
        other.source_ = Span<const uint8>();
        other.position_ = 0;
    }

    return *this;
}

Result<ArchiveReader> ArchiveReader::Create(Span<const uint8> source,
                                            ArchiveLimits limits) noexcept {
    return CreateShared(source, limits, SharedPtr<ArchiveBudget>());
}

Result<ArchiveReader> ArchiveReader::CreateShared(Span<const uint8> source, ArchiveLimits limits,
                                                  SharedPtr<ArchiveBudget> shared) noexcept {
    if (source.Size() > limits.maxTotalBytes) {
        return Result<ArchiveReader>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<ArchiveReader>::Success(ArchiveReader(source, limits, Move(shared)));
}

Result<void> ArchiveReader::Spend(uint64 work) noexcept {
    if (shared_) {
        return shared_->Spend(work);
    }

    if (work > limits_.maxWork - work_) {
        work_ = limits_.maxWork;
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    work_ += work;
    return Result<void>::Success();
}

Result<void> ArchiveReader::Charge(uint64 work) noexcept {
    if (shared_) {
        return shared_->Charge(work);
    }

    if (operations_ == limits_.maxOperations) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    ++operations_;
    return Spend(work);
}

Result<Span<const uint8>> ArchiveReader::Fixed(usize count) noexcept {
    auto charged = Charge(1);
    if (!charged) {
        return Result<Span<const uint8>>::Failure(charged.Error());
    }

    if (count > Remaining()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    auto spent = Spend(static_cast<uint64>(count));
    if (!spent) {
        return Result<Span<const uint8>>::Failure(spent.Error());
    }

    const uint8* begin = source_.Data();
    if (position_ != 0) {
        begin += position_;
    }

    return Result<Span<const uint8>>::Success({begin, count});
}

Result<Span<const uint8>> ArchiveReader::Variable(bool utf8, bool ownedCopy) noexcept {
    auto prefix = Fixed(8);
    if (!prefix) {
        return Result<Span<const uint8>>::Failure(prefix.Error());
    }

    const uint64 length = LoadLittleEndian<uint64>(prefix.Value().Data());
    if (length > limits_.maxValueBytes) {
        return Result<Span<const uint8>>::Failure(ErrorCode::CapacityExceeded);
    }

    if (length > Remaining() - 8) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    // borrowed bytes도 크기에 비례한 budget을 청구하여 scan/copy 구현 간 한도가 느슨해지지 않는다.
    auto spent = Spend(length);
    if (!spent) {
        return Result<Span<const uint8>>::Failure(spent.Error());
    }

    if (ownedCopy) {
        // String::TryAssign의 validation과 copy도 별도로 선불 청구한다.
        spent = Spend(length);
        if (!spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }

        spent = Spend(length);
        if (!spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }
    }

    const auto count = static_cast<usize>(length);
    const uint8* data = source_.Data() + position_ + 8;
    if (utf8) {
        auto valid = StringView(reinterpret_cast<const char*>(data), count).ValidateUtf8();
        if (!valid) {
            return Result<Span<const uint8>>::Failure(valid.Error());
        }
    }

    return Result<Span<const uint8>>::Success({data, count});
}

Result<bool> ArchiveReader::ReadBool() noexcept {
    auto bytes = Fixed(1);
    if (!bytes) {
        return Result<bool>::Failure(bytes.Error());
    }

    if (bytes.Value()[0] > 1) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    ++position_;
    return Result<bool>::Success(bytes.Value()[0] != 0);
}

Result<uint32> ArchiveReader::ReadU32() noexcept {
    auto bytes = Fixed(4);
    if (!bytes) {
        return Result<uint32>::Failure(bytes.Error());
    }

    const uint32 value = LoadLittleEndian<uint32>(bytes.Value().Data());
    position_ += 4;
    return Result<uint32>::Success(value);
}

Result<uint64> ArchiveReader::ReadU64() noexcept {
    auto bytes = Fixed(8);
    if (!bytes) {
        return Result<uint64>::Failure(bytes.Error());
    }

    const uint64 value = LoadLittleEndian<uint64>(bytes.Value().Data());
    position_ += 8;
    return Result<uint64>::Success(value);
}

Result<int64> ArchiveReader::ReadI64() noexcept {
    auto value = ReadU64();
    if (!value) {
        return Result<int64>::Failure(value.Error());
    }

    // C++23의 two's-complement 결과를 값 범위 밖 unsigned→signed cast 없이 재현한다.
    const uint64 bits = value.Value();
    const int64 result =
        (bits >> 63) == 0 ? static_cast<int64>(bits) : -1 - static_cast<int64>(~bits);
    return Result<int64>::Success(result);
}

Result<float> ArchiveReader::ReadF32() noexcept {
    auto value = ReadU32();
    if (!value) {
        return Result<float>::Failure(value.Error());
    }

    return Result<float>::Success(platform::BitCast<float>(value.Value()));
}

Result<double> ArchiveReader::ReadF64() noexcept {
    auto value = ReadU64();
    if (!value) {
        return Result<double>::Failure(value.Error());
    }

    return Result<double>::Success(platform::BitCast<double>(value.Value()));
}

Result<Guid> ArchiveReader::ReadGuid() noexcept {
    auto bytes = Fixed(16);
    if (!bytes) {
        return Result<Guid>::Failure(bytes.Error());
    }

    Guid value{LoadLittleEndian<uint64>(bytes.Value().Data()),
               LoadLittleEndian<uint64>(bytes.Value().Data() + 8)};
    position_ += 16;
    return Result<Guid>::Success(value);
}

Result<Span<const uint8>> ArchiveReader::ReadBytes() noexcept {
    auto value = Variable(false, false);
    if (value) {
        position_ += 8 + value.Value().Size();
    }

    return value;
}

Result<StringView> ArchiveReader::ReadStringView() noexcept {
    auto value = Variable(true, false);
    if (!value) {
        return Result<StringView>::Failure(value.Error());
    }

    position_ += 8 + value.Value().Size();
    return Result<StringView>::Success(
        {reinterpret_cast<const char*>(value.Value().Data()), value.Value().Size()});
}

Result<String> ArchiveReader::ReadString(Allocator& allocator) noexcept {
    auto value = Variable(true, true);
    if (!value) {
        return Result<String>::Failure(value.Error());
    }

    String result(allocator);
    auto assigned = result.TryAssign(
        {reinterpret_cast<const char*>(value.Value().Data()), value.Value().Size()});
    if (!assigned) {
        return Result<String>::Failure(assigned.Error());
    }

    position_ += 8 + value.Value().Size();
    return Result<String>::Success(Move(result));
}

Result<Span<const uint8>> ArchiveReader::ReadRaw(usize count) noexcept {
    if (count > limits_.maxValueBytes) {
        auto charged = Charge(1);
        return Result<Span<const uint8>>::Failure(charged ? ErrorCode::CapacityExceeded
                                                          : charged.Error());
    }

    auto value = Fixed(count);
    if (value) {
        position_ += count;
    }

    return value;
}

usize ArchiveReader::Position() const noexcept {
    return position_;
}

usize ArchiveReader::Remaining() const noexcept {
    return source_.Size() - position_;
}

bool ArchiveReader::AtEnd() const noexcept {
    return position_ == source_.Size();
}

uint64 ArchiveReader::OperationsUsed() const noexcept {
    return shared_ ? shared_->OperationsUsed() : operations_;
}

uint64 ArchiveReader::WorkUsed() const noexcept {
    return shared_ ? shared_->WorkUsed() : work_;
}

} // namespace rocks
