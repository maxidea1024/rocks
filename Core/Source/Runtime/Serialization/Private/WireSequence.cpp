#include "WireInternal.h"

#include <Base/Checked.h>

// 원소 열(ValueArray·Set·Map) header 검사·count 사전 검사·exact 검사와 writer 검사다
// (WireInternal.h).
namespace rocks::detail {
namespace {

constexpr usize MaxColumns = 2; // Map의 키·값

// count개의 원소가 차지하는 최소 byte다. usize를 넘으면 false다.
bool ElementBytes(uint32 count, usize elementBytes, usize& total) noexcept {
    if (elementBytes != 0 && count > MaxSize / elementBytes) {
        return false;
    }

    total = static_cast<usize>(count) * elementBytes;
    return true;
}

// 열들의 shape를 풀어 resolved에 넣고, 원소 하나(열들을 합친 것)의 크기와 길이 prefix가 있는 열의
// 유무를 돌려준다. 열 수가 1–2가 아니거나 그 자리의 tag가 아니거나 모순 조합이거나 최소 크기 합이
// usize를 넘으면 InvalidArgument다.
Result<WireValueShape> ResolveColumns(Span<const WireColumn> columns, WireValueShape* resolved,
                                      bool& anyPrefix) noexcept {
    if (columns.Size() == 0 || columns.Size() > MaxColumns) {
        return Result<WireValueShape>::Failure(ErrorCode::InvalidArgument);
    }

    WireValueShape entry{0, true};
    anyPrefix = false;
    for (usize index = 0; index < columns.Size(); ++index) {
        auto shape =
            ResolveSlotShape(columns[index].slot, columns[index].wire, columns[index].shape);
        if (!shape) {
            return shape;
        }

        resolved[index] = shape.Value();
        if (!TryAdd(entry.minElementBytes, shape.Value().minElementBytes, entry.minElementBytes)) {
            return Result<WireValueShape>::Failure(ErrorCode::InvalidArgument);
        }

        entry.exact = entry.exact && shape.Value().exact;
        usize offset = 0;
        usize fixedBytes = 0;
        anyPrefix = anyPrefix || LengthPrefix(columns[index].wire, offset, fixedBytes);
    }

    return Result<WireValueShape>::Success(entry);
}

} // namespace

Result<WireSequence> OpenWireSequence(ArchiveReader&& payload, Span<const WireColumn> columns,
                                      uint32 maxCount) noexcept {
    WireValueShape resolved[MaxColumns];
    bool anyPrefix = false;
    auto entry = ResolveColumns(columns, resolved, anyPrefix);
    if (!entry) {
        return Result<WireSequence>::Failure(entry.Error()); // payload는 그대로다
    }

    ArchiveReader archive = Move(payload);
    uint32 stored[MaxColumns] = {};
    for (usize index = 0; index < columns.Size(); ++index) {
        auto tag = archive.ReadU32();
        if (!tag) {
            return Result<WireSequence>::Failure(tag.Error());
        }

        stored[index] = tag.Value();
    }

    // 그 자리가 받지 않는 tag(중첩 컨테이너·Record·모르는 값)는 형식 오류이고, 받는 tag끼리 다르면
    // 형식 불일치다.
    for (usize index = 0; index < columns.Size(); ++index) {
        if (!WireSlotShape(columns[index].slot, stored[index])) {
            return Result<WireSequence>::Failure(ErrorCode::CorruptData);
        }
    }

    for (usize index = 0; index < columns.Size(); ++index) {
        if (stored[index] != columns[index].wire) {
            return Result<WireSequence>::Failure(ErrorCode::TypeMismatch);
        }
    }

    auto count = archive.ReadU32();
    if (!count) {
        return Result<WireSequence>::Failure(count.Error());
    }

    if (count.Value() > maxCount) {
        return Result<WireSequence>::Failure(ErrorCode::CapacityExceeded);
    }

    // 외부 count만 믿고 원소를 읽거나 호출자가 할당하지 않게, 남은 byte가 원소 최소 크기를 담는지
    // 먼저 본다(모든 열이 고정 폭·exact면 정확히 같아야 한다).
    const usize entryBytes = entry.Value().minElementBytes;
    const bool exact = entry.Value().exact;
    usize required = 0;
    if (!ElementBytes(count.Value(), entryBytes, required) || required > archive.Remaining() ||
        (exact && required != archive.Remaining())) {
        return Result<WireSequence>::Failure(ErrorCode::CorruptData);
    }

    // count만큼의 work를 원소 검사·읽기 전에 청구한다(아래 exact 검사의 원소당 비교도 덮는다).
    if (auto spent = WireAccess::Spend(archive, count.Value()); !spent) {
        return Result<WireSequence>::Failure(spent.Error());
    }

    // 가변 wire의 exact: 합계만 맞고 원소 경계가 어긋난 입력(예: Digest 길이 0과 64)을 거부한다.
    // 남은 byte가 정확히 count × 원소 크기이므로 원소 i는 i × 원소 크기에서 시작한다. 모든 열이
    // 고정 폭이면(길이 prefix가 없으면) 볼 것이 없으므로 훑지 않는다.
    if (exact && anyPrefix) {
        const uint8* entries = WireAccess::Rest(archive).Data();
        for (uint32 element = 0; element < count.Value(); ++element) {
            usize at = static_cast<usize>(element) * entryBytes;
            for (usize index = 0; index < columns.Size(); ++index) {
                if (!LengthPrefixMatches(columns[index].wire, resolved[index], entries + at)) {
                    return Result<WireSequence>::Failure(ErrorCode::CorruptData);
                }

                at += resolved[index].minElementBytes;
            }
        }
    }

    return Result<WireSequence>::Success(WireSequence{Move(archive), count.Value()});
}

Result<WireValueShape> BeginWireSequence(ArchiveWriter& out, Span<const WireColumn> columns,
                                         uint32 count, usize maxBytes, uint32 maxCount) noexcept {
    WireValueShape resolved[MaxColumns];
    bool anyPrefix = false;
    auto entry = ResolveColumns(columns, resolved, anyPrefix);
    if (!entry) {
        return entry;
    }

    const usize headerBytes = WireSequenceHeaderBytes(columns.Size());
    usize elements = 0;
    if (count > maxCount || maxBytes < headerBytes ||
        !ElementBytes(count, entry.Value().minElementBytes, elements) ||
        elements > maxBytes - headerBytes) {
        return Result<WireValueShape>::Failure(ErrorCode::CapacityExceeded);
    }

    for (usize index = 0; index < columns.Size(); ++index) {
        if (auto written = out.WriteU32(columns[index].wire); !written) {
            return Result<WireValueShape>::Failure(written.Error());
        }
    }

    if (auto written = out.WriteU32(count); !written) {
        return Result<WireValueShape>::Failure(written.Error());
    }

    return entry;
}

Result<void> CheckWireSequence(const ArchiveWriter& out, usize start, usize maxBytes) noexcept {
    if (out.ByteCount() < start) {
        return Result<void>::Failure(ErrorCode::InvalidArgument); // 호출자가 out을 줄였다
    }

    if (out.ByteCount() - start > maxBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

Result<void> EndWireSequence(const ArchiveWriter& out, usize start, usize headerBytes, uint32 count,
                             WireValueShape entry, usize maxBytes) noexcept {
    if (out.ByteCount() < start) {
        return Result<void>::Failure(ErrorCode::InvalidArgument); // 호출자가 out을 줄였다
    }

    const usize total = out.ByteCount() - start;
    if (total > maxBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // Begin이 maxBytes 안임을 확인했으므로 넘치지 않는다. 가변 원소는 합계의 하한만 본다.
    const usize minimum = headerBytes + static_cast<usize>(count) * entry.minElementBytes;
    if (total < minimum || (entry.exact && total != minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::detail
