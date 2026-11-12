#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>

namespace rocks {

// work는 byte 처리량과 탐색 비교의 보수적 비용이다. 실패 재시도도 operation을 소비한다.
// maxValueBytes는 가변 bytes/string/raw 범위에 적용하며 고정 폭 primitive에는 적용하지 않는다.
struct ArchiveLimits {
    usize maxTotalBytes = 16 * 1024 * 1024;
    usize maxValueBytes = 4 * 1024 * 1024;
    uint64 maxOperations = 1024 * 1024;
    uint64 maxWork = 64 * 1024 * 1024;
};

class RecordBuilder;
class RecordReader;

// 고정 폭 값은 little-endian으로 저장한다. bool은 0/1, i64는 two's-complement, float는 IEEE bits다.
// Guid는 high 다음 low를 각각 u64로 쓴다. bytes/string은 u64 byteLength+payload이며 종단 NUL은
// 없다. allocator는 writer와 TakeBytes로 이전한 buffer보다 오래 살아야 한다. 동시 변경/allocator
// 재진입은 금지다.
class ArchiveWriter {
public:
    explicit ArchiveWriter(Allocator&, ArchiveLimits = {}) noexcept;
    ArchiveWriter(ArchiveWriter&&) noexcept = default;
    ArchiveWriter& operator=(ArchiveWriter&&) noexcept = default;
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    // 실패하면 기존 byte/길이는 불변이다. 검사에 소비한 operation/work 예산은 환급하지 않는다.
    // 자기 View의 전체/부분 입력도 허용한다. 성공한 성장·TakeBytes·소멸 뒤 이전 뷰는 보관하지
    // 않는다.
    Result<void> WriteBool(bool) noexcept;
    Result<void> WriteU32(uint32) noexcept;
    Result<void> WriteU64(uint64) noexcept;
    Result<void> WriteI64(int64) noexcept;
    Result<void> WriteF32(float) noexcept;
    Result<void> WriteF64(double) noexcept;
    Result<void> WriteGuid(Guid) noexcept;
    Result<void> WriteBytes(Span<const uint8>) noexcept;
    Result<void> WriteString(StringView) noexcept;
    // 길이 prefix 없는 명시 byte 기록이다. 구조체의 host layout을 저장하는 용도로 사용하지 않는다.
    Result<void> WriteRaw(Span<const uint8>) noexcept;

    Span<const uint8> View() const noexcept;
    usize ByteCount() const noexcept;
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;
    // buffer 소유권만 이전하고 누적 예산은 유지한다. 반환 Array도 같은 allocator를 빌린다.
    Array<uint8> TakeBytes() noexcept;

private:
    Result<void> Charge(uint64 work) noexcept;
    Result<void> Append(Span<const uint8> prefix, Span<const uint8> payload,
                        bool utf8 = false) noexcept;
    Result<void> AppendReserved(Span<const uint8>, Span<const uint8>) noexcept;
    Array<uint8> bytes_;
    ArchiveLimits limits_;
    uint64 operations_ = 0;
    uint64 work_ = 0;
    friend class RecordBuilder;
};

// 입력을 빌리는 이동 전용 cursor다. 원본 범위는 reader/반환 view보다 오래 살아 있고 변경되지 않아야
// 한다. pointer/count가 실제 readable 범위인지는 caller 전제다. 읽기 API는 같은 reader에서 병행하지
// 않는다.
class ArchiveReader {
public:
    static Result<ArchiveReader> Create(Span<const uint8>, ArchiveLimits = {}) noexcept;
    ArchiveReader(ArchiveReader&&) noexcept = default;
    ArchiveReader& operator=(ArchiveReader&&) noexcept = default;
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    // 실패는 cursor를 전혀 이동하지 않는다. 예산은 실패 시에도 이미 소비한 비용을 유지한다.
    // truncation/잘못된 bool/UTF-8은 InvalidArgument, 크기·작업 한도는 CapacityExceeded다.
    Result<bool> ReadBool() noexcept;
    Result<uint32> ReadU32() noexcept;
    Result<uint64> ReadU64() noexcept;
    Result<int64> ReadI64() noexcept;
    Result<float> ReadF32() noexcept;
    Result<double> ReadF64() noexcept;
    Result<Guid> ReadGuid() noexcept;
    Result<Span<const uint8>> ReadBytes() noexcept;
    Result<StringView> ReadStringView() noexcept;
    Result<String> ReadString(Allocator&) noexcept;
    Result<Span<const uint8>> ReadRaw(usize byteCount) noexcept;

    usize Position() const noexcept;
    usize Remaining() const noexcept;
    bool AtEnd() const noexcept;
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    ArchiveReader(Span<const uint8>, ArchiveLimits) noexcept;
    Result<void> Charge(uint64 work) noexcept;
    Result<void> Spend(uint64 work) noexcept;
    Result<Span<const uint8>> Fixed(usize) noexcept;
    Result<Span<const uint8>> Variable(bool utf8, bool ownedCopy) noexcept;
    Span<const uint8> source_;
    ArchiveLimits limits_;
    usize position_ = 0;
    uint64 operations_ = 0;
    uint64 work_ = 0;
    friend class RecordReader;
};

} // namespace rocks
