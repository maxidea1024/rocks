#include <Crypto/SegmentStream.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AeadInternal.h"
#include "KeyAccess.h"
#include "SegmentStreamInternal.h"

#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 임의 접근 복호(SegmentStreamRandomAccess)와 병렬 봉인(SegmentStreamParallelWriter)이다. 둘 다
// 생성 뒤 바뀌지 않는 세그먼트 키(AeadKey)를 여러 스레드가 함께 읽는다. 병렬 봉인의 index claim은
// 비트맵의 원자적 OR이다.
namespace rocks::crypto {
namespace {

constexpr uint64 ClaimBits = 64;

// 세그먼트 index의 암호문 크기(마지막을 뺀 세그먼트는 S, 마지막은 r)다.
uint64 SegmentCiphertextBytes(const SegmentLayout& layout, uint64 index) noexcept {
    return index + 1 == layout.segmentCount ? layout.lastSegmentBytes : layout.segmentBytes;
}

bool SegmentStreamRandomAccessBadOverlap(Span<const uint8> input, Span<uint8> output) noexcept {
    return Overlaps(input, output) && input.Data() != output.Data();
}

} // namespace

// ---- SegmentStreamRandomAccess ----

Result<SegmentStreamRandomAccess>
SegmentStreamRandomAccess::TryOpen(const SegmentStreamKey& key, Span<const uint8> associatedData,
                                   Span<const uint8> header, Span<const uint8> lastSegment,
                                   uint64 ciphertextBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<SegmentStreamRandomAccess>::Failure(ErrorCode::StaleHandle);
    }

    // 헤더는 지역 사본으로 한 번만 읽는다(공유 메모리가 호출 중에 바뀌어도 판정과 파생이 같은
    // byte를 본다). 길이가 틀리면 원본으로 형식 오류를 판정한다.
    uint8 copy[SegmentHeaderBytes];
    Span<const uint8> stable = header;
    if (header.Size() == SegmentHeaderBytes) {
        platform::MemoryCopy(copy, header.Data(), SegmentHeaderBytes);
        stable = Span<const uint8>(copy);
    }

    uint8 associatedHash[detail::SegmentAssociatedHashBytes];
    detail::HashSegmentAssociatedData(associatedData, Span<uint8>(associatedHash));
    SegmentStreamParams params;
    auto segmentKey = detail::TryOpenSegmentStream(
        detail::KeyAccess::Bytes(key), key.Id(), Span<const uint8>(associatedHash), stable, params);
    if (!segmentKey) {
        return Result<SegmentStreamRandomAccess>::Failure(segmentKey.Error());
    }

    auto layout = TryGetSegmentLayout(params, ciphertextBytes);
    if (!layout) {
        return Result<SegmentStreamRandomAccess>::Failure(layout.Error());
    }

    const SegmentLayout& found = layout.Value();
    if (static_cast<uint64>(lastSegment.Size()) != found.lastSegmentBytes ||
        !detail::VerifySegment(segmentKey.Value(), found.segmentCount - 1, true, lastSegment)) {
        return Result<SegmentStreamRandomAccess>::Failure(ErrorCode::AuthenticationFailed);
    }

    SegmentStreamRandomAccess reader;
    reader.key_ = Move(segmentKey.Value());
    reader.layout_ = found;
    return Result<SegmentStreamRandomAccess>::Success(Move(reader));
}

Result<SegmentRange> SegmentStreamRandomAccess::TryMapPlaintextRange(uint64 offset,
                                                                     uint64 bytes) const noexcept {
    if (key_.IsEmpty()) {
        return Result<SegmentRange>::Failure(ErrorCode::StaleHandle);
    }

    if (offset > layout_.plaintextBytes || bytes > layout_.plaintextBytes - offset) {
        return Result<SegmentRange>::Failure(ErrorCode::InvalidArgument);
    }

    SegmentRange range;
    if (bytes == 0) {
        return Result<SegmentRange>::Success(range);
    }

    const uint64 capacity = layout_.segmentBytes - SegmentTagBytes;
    const uint64 last = (offset + bytes - 1) / capacity;
    range.firstSegment = offset / capacity;
    range.segmentCount = last - range.firstSegment + 1;
    range.ciphertextOffset = SegmentHeaderBytes + range.firstSegment * layout_.segmentBytes;
    const uint64 end = last + 1 == layout_.segmentCount
                           ? layout_.lastSegmentOffset + layout_.lastSegmentBytes
                           : SegmentHeaderBytes + (last + 1) * layout_.segmentBytes;
    range.ciphertextBytes = end - range.ciphertextOffset;
    range.firstSegmentOffset = offset - range.firstSegment * capacity;
    return Result<SegmentRange>::Success(range);
}

Result<usize> SegmentStreamRandomAccess::TryOpenSegment(uint64 index,
                                                        Span<const uint8> ciphertextSegment,
                                                        Span<uint8> plaintext) const noexcept {
    platform::DataIndependentTimingScope dit;
    if (key_.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (index >= layout_.segmentCount ||
        SegmentStreamRandomAccessBadOverlap(ciphertextSegment, plaintext)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 expected = SegmentCiphertextBytes(layout_, index);
    const usize plaintextBytes = static_cast<usize>(expected - SegmentTagBytes);
    if (plaintext.Size() < plaintextBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // 암호문 길이는 메시지에서 온 값이다(잘린 세그먼트).
    if (static_cast<uint64>(ciphertextSegment.Size()) != expected ||
        !detail::OpenSegment(key_, index, index + 1 == layout_.segmentCount, ciphertextSegment,
                             plaintext.Left(plaintextBytes))) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(plaintextBytes);
}

// ---- SegmentStreamParallelWriter ----

Result<SegmentStreamParallelWriter> detail::SegmentStreamFactory::TryCreateParallelWriter(
    const SegmentStreamKey& key, const SegmentStreamParams& params,
    Span<const uint8> associatedData, uint64 plaintextBytes, Span<const uint8> salt,
    Span<uint8> header, Allocator& bufferAllocator) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<SegmentStreamParallelWriter>::Failure(ErrorCode::StaleHandle);
    }

    if (!IsValidSegmentParams(params) || header.Size() != SegmentHeaderBytes ||
        salt.Size() != SegmentSaltBytes) {
        return Result<SegmentStreamParallelWriter>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto usable = CheckAeadUsable(SegmentAeadAlgorithm(params.algorithm)); !usable) {
        return Result<SegmentStreamParallelWriter>::Failure(usable.Error());
    }

    if (plaintextBytes > SegmentMaxPlaintextBytes(params)) {
        return Result<SegmentStreamParallelWriter>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 made[SegmentHeaderBytes];
    auto segmentKey = TryStartSegmentStream(key, params, associatedData, salt, Span<uint8>(made));
    if (!segmentKey) {
        return Result<SegmentStreamParallelWriter>::Failure(segmentKey.Error());
    }

    const SegmentLayout layout = SegmentLayoutForPlaintext(params, plaintextBytes);
    const usize words = static_cast<usize>((layout.segmentCount + ClaimBits - 1) / ClaimBits);
    auto bitmap = bufferAllocator.Allocate(words * sizeof(uint64), alignof(uint64));
    if (!bitmap) {
        return Result<SegmentStreamParallelWriter>::Failure(bitmap.Error());
    }

    platform::MemoryZero(bitmap.Value(), words * sizeof(uint64));
    platform::MemoryCopy(header.Data(), made, SegmentHeaderBytes);
    SegmentStreamParallelWriter writer;
    writer.key_ = Move(segmentKey.Value());
    writer.layout_ = layout;
    writer.allocator_ = &bufferAllocator;
    writer.claimed_ = static_cast<uint64*>(bitmap.Value());
    writer.claimedWords_ = words;
    return Result<SegmentStreamParallelWriter>::Success(Move(writer));
}

SegmentStreamParallelWriter::~SegmentStreamParallelWriter() noexcept {
    Release();
}

SegmentStreamParallelWriter::SegmentStreamParallelWriter(
    SegmentStreamParallelWriter&& other) noexcept
    : key_(Move(other.key_)), layout_(other.layout_), allocator_(other.allocator_),
      claimed_(other.claimed_), claimedWords_(other.claimedWords_) {
    other.layout_ = SegmentLayout();
    other.allocator_ = nullptr;
    other.claimed_ = nullptr;
    other.claimedWords_ = 0;
}

SegmentStreamParallelWriter&
SegmentStreamParallelWriter::operator=(SegmentStreamParallelWriter&& other) noexcept {
    if (this != &other) {
        Release();
        key_ = Move(other.key_);
        layout_ = other.layout_;
        allocator_ = other.allocator_;
        claimed_ = other.claimed_;
        claimedWords_ = other.claimedWords_;
        other.layout_ = SegmentLayout();
        other.allocator_ = nullptr;
        other.claimed_ = nullptr;
        other.claimedWords_ = 0;
    }

    return *this;
}

void SegmentStreamParallelWriter::Release() noexcept {
    key_ = AeadKey();
    if (claimed_ != nullptr) {
        platform::MemoryZero(claimed_, claimedWords_ * sizeof(uint64));
        if (!allocator_->Deallocate(claimed_)) {
            platform::Fatal("Crypto: segment claim bitmap was not owned by its allocator");
        }
    }

    layout_ = SegmentLayout();
    allocator_ = nullptr;
    claimed_ = nullptr;
    claimedWords_ = 0;
}

Result<SegmentStreamParallelWriter>
SegmentStreamParallelWriter::TryCreate(const SegmentStreamKey& key,
                                       const SegmentStreamParams& params,
                                       Span<const uint8> associatedData, uint64 plaintextBytes,
                                       Span<uint8> header, Allocator& bufferAllocator) noexcept {
    platform::DataIndependentTimingScope dit;
    uint8 salt[detail::SegmentSaltBytes] = {};
    if (!key.IsEmpty() && detail::IsValidSegmentParams(params) &&
        header.Size() == SegmentHeaderBytes) {
        if (auto filled = TryFillRandom(Span<uint8>(salt)); !filled) {
            return Result<SegmentStreamParallelWriter>::Failure(filled.Error());
        }
    }

    return detail::SegmentStreamFactory::TryCreateParallelWriter(
        key, params, associatedData, plaintextBytes, Span<const uint8>(salt), header,
        bufferAllocator);
}

Result<usize> SegmentStreamParallelWriter::TrySealSegment(uint64 index, Span<const uint8> plaintext,
                                                          Span<uint8> sealed) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key_.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (index >= layout_.segmentCount) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize sealedBytes = static_cast<usize>(SegmentCiphertextBytes(layout_, index));
    const usize plaintextBytes = sealedBytes - SegmentTagBytes;
    if (plaintext.Size() != plaintextBytes ||
        SegmentStreamRandomAccessBadOverlap(plaintext, sealed)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (sealed.Size() < sealedBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // claim 뒤에는 실패 경로가 없다(봉인은 길이를 검사한 libsodium 호출이다).
    const uint64 mask = uint64{1} << (index % ClaimBits);
    const uint64 previous =
        platform::AtomicFetchOr(claimed_ + static_cast<usize>(index / ClaimBits), mask);
    if ((previous & mask) != 0) {
        return Result<usize>::Failure(ErrorCode::AlreadyExists);
    }

    detail::SealSegment(key_, index, index + 1 == layout_.segmentCount, plaintext,
                        sealed.Left(sealedBytes));
    return Result<usize>::Success(sealedBytes);
}

} // namespace rocks::crypto
