#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SharedPtr.h>
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
class RecordListReader;
class RecordListWriter;
class ValueArrayReader;

// 여러 reader가 함께 쓰는 누적 operation·work 예산과 그 한도다. 한 Record 트리(RecordReader와
// 그것이 연 값 reader·중첩 목록·항목 reader)가 하나를 참조 계수로 공유한다(SharedPtr — 어느
// reader가 먼저 파괴돼도 된다). 값·항목마다 새 예산을 주지 않으므로 "소비한 예산은 환급하지 않는다"
// 원칙이 트리 전체에 적용된다. 한도를 예산이 들고 있어 공유자가 서로 다른 한도로 판정하지 않는다.
// 사용량은 원자적으로 누적하므로 같은 예산을 공유하는 서로 다른 reader를 여러 스레드에서 써도
// 된다(같은 reader의 병행은 여전히 금지).
class ArchiveBudget {
public:
    ArchiveBudget(uint64 maxOperations, uint64 maxWork) noexcept
        : maxOperations_(maxOperations), maxWork_(maxWork) {
    }

    ArchiveBudget(const ArchiveBudget&) = delete;
    ArchiveBudget& operator=(const ArchiveBudget&) = delete;

    // operation 하나와 work를 청구한다. 넘으면 CapacityExceeded다. work 한도를 넘으면 사용량을
    // 한도로 포화하므로 이후 work가 0이 아닌 청구는 모두 실패한다(사용량은 줄지 않는다). work가 0인
    // Spend는 언제나 성공하고, work가 0인 Charge는 operation 한도로만 판정한다.
    Result<void> Charge(uint64 work) noexcept;
    Result<void> Spend(uint64 work) noexcept;
    uint64 OperationsUsed() const noexcept;
    uint64 WorkUsed() const noexcept;

private:
    uint64 maxOperations_;
    uint64 maxWork_;
    uint64 operations_ = 0;
    uint64 work_ = 0;
};

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
    // 이미 쓴 범위 [offset, offset + bytes.Size())를 덮어쓴다(RecordWriter·RecordListWriter의
    // 길이·개수 채우기).
    Result<void> Patch(usize offset, Span<const uint8> bytes) noexcept;
    Array<uint8> bytes_;
    ArchiveLimits limits_;
    uint64 operations_ = 0;
    uint64 work_ = 0;
    friend class RecordBuilder;
    friend class RecordWriter;
    friend class RecordListWriter;
};

// 입력을 빌리는 이동 전용 cursor다. 원본 범위는 reader/반환 view보다 오래 살아 있고 변경되지 않아야
// 한다. pointer/count가 실제 readable 범위인지는 caller 전제다. 읽기 API는 같은 reader에서 병행하지
// 않는다. Create로 만든 reader는 자기 예산을, RecordReader::OpenValue로 연 reader는 그 Record
// 트리의 공유 예산을 쓴다. 이동된 원본은 빈 입력이 된다(남은 예산으로 같은 입력을 다시 읽어 한도를
// 늘리지 못한다).
class ArchiveReader {
public:
    static Result<ArchiveReader> Create(Span<const uint8>, ArchiveLimits = {}) noexcept;
    ArchiveReader(ArchiveReader&& other) noexcept;
    ArchiveReader& operator=(ArchiveReader&& other) noexcept;
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
    ArchiveReader(Span<const uint8>, ArchiveLimits, SharedPtr<ArchiveBudget> shared) noexcept;
    // shared가 비어 있지 않으면 그 누적 예산(과 그 한도)을 쓴다.
    static Result<ArchiveReader> CreateShared(Span<const uint8>, ArchiveLimits,
                                              SharedPtr<ArchiveBudget> shared) noexcept;
    Result<void> Charge(uint64 work) noexcept;
    Result<void> Spend(uint64 work) noexcept;
    Result<Span<const uint8>> Fixed(usize) noexcept;
    Result<Span<const uint8>> Variable(bool utf8, bool ownedCopy) noexcept;
    Span<const uint8> source_;
    ArchiveLimits limits_;
    usize position_ = 0;
    uint64 operations_ = 0;
    uint64 work_ = 0;
    SharedPtr<ArchiveBudget> shared_;
    friend class RecordReader;
    friend class RecordListReader;
    friend class ValueArrayReader;
};

} // namespace rocks
