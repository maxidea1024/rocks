#pragma once

#include <Serialization/WireSlot.h>

// Serialization 모듈 안의 Record·RecordList와 wire reader·writer(ValueArray·Optional·Set·Map·
// InstancedStruct·Text)가 함께 쓰는 내부 도구다. 공개 헤더가 아니다.
namespace rocks::detail {

// Record envelope header의 byte 수다(magic 8 + envelope version 4 + schema 16 + schema version 4 +
// field count 4 + payload byte 8).
inline constexpr usize RecordHeaderBytes = 44;

// 길이 prefix(u64)의 byte 수다(String·Bytes·RecordList 항목·원소 자리 길이 prefix Record).
inline constexpr usize LengthPrefixBytes = 8;

// Archive·Record 내부에 닿는 통로다(ArchiveReader·ArchiveWriter·RecordReader의 friend).
struct WireAccess {
    // reader의 지금 위치부터 끝까지의 byte다(청구하지 않는다).
    static Span<const uint8> Rest(const ArchiveReader& reader) noexcept {
        return Span<const uint8>(reader.source_.Data() + reader.position_, reader.Remaining());
    }

    // 실패한 복합 읽기가 cursor를 호출 전 위치로 되돌린다(소비한 예산은 환급하지 않는다).
    static void Rewind(ArchiveReader& reader, usize position) noexcept {
        reader.position_ = position;
    }

    // count만큼의 work를 청구한다(원소 수 선청구).
    static Result<void> Spend(ArchiveReader& reader, uint64 work) noexcept {
        return reader.Spend(work);
    }

    // 지금 위치부터 count byte를 내용으로 넘기고 그 범위를 돌려준다. cursor는 성공할 때만 옮긴다.
    // operation 하나를 청구하고, 내용 byte는 단독 reader일 때만 청구한다. Record 트리의 공유 예산
    // reader(RecordReader::OpenValue·각 reader의 OpenField)면 그 입력 byte를 트리의 Open이 이미
    // 청구했다. count가 maxValueBytes를 넘으면 CapacityExceeded, 남은 byte보다 크면
    // CorruptData다(읽은 데이터 손상 — AssetSystem 19장).
    static Result<Span<const uint8>> SkipContent(ArchiveReader& reader, usize count) noexcept;

    // u64 길이 + 내용 하나를 넘기고 내용 범위를 돌려준다. 청구는 operation 하나와 길이 prefix 8이고
    // 내용 byte는 SkipContent와 같은 규칙이다. 길이 prefix가 잘렸거나 길이가 남은 byte보다 크면
    // CorruptData, maxValueBytes를 넘으면 CapacityExceeded다. cursor는 성공할 때만 옮긴다.
    static Result<Span<const uint8>> ReadPrefixedContent(ArchiveReader& reader) noexcept;

    // parent가 돌려준 field(wire tag가 wire와 같아야 한다)를 parent와 같은 예산·한도의 값 reader로
    // 연다. operation 하나를 청구하며 이동된 parent·parent 밖의 범위는 InvalidArgument, wire
    // 불일치는 TypeMismatch다(ValueArrayReader::OpenField와 같은 검사 순서).
    static Result<ArchiveReader> OpenFieldValue(const RecordReader& parent, const FieldView& field,
                                                uint32 wire) noexcept;

    // 이미 쓴 범위를 덮어쓴다(길이 채우기).
    static Result<void> Patch(ArchiveWriter& out, usize offset, Span<const uint8> bytes) noexcept {
        return out.Patch(offset, bytes);
    }

    // prefix와 payload를 한 번에 붙인다(ArchiveWriter의 Write*와 같은 경로 — 검사·청구 한 번,
    // utf8이면 payload의 UTF-8 검사, payload가 out 자신의 View를 가리켜도 성장 뒤 위치를 보정,
    // 실패하면 byte 불변). 여러 칸으로 된 값(Text)을 쓰는 데 쓴다.
    static Result<void> Append(ArchiveWriter& out, Span<const uint8> prefix,
                               Span<const uint8> payload, bool utf8) noexcept {
        return out.Append(prefix, payload, utf8);
    }
};

// 원소 열(ValueArray·Set 한 열, Map 키·값 두 열)의 열 하나다.
struct WireColumn {
    WireSlot slot;
    uint32 wire;
    WireValueShape shape;
};

// 연 원소 열이다: archive는 header 바로 뒤(첫 원소)에 놓인 값 reader, count는 header의 원소 수다.
struct WireSequence {
    ArchiveReader archive;
    uint32 count = 0;
};

// 자리·wire·schema shape를 합친 크기다(reader·writer 공통). 그 자리의 tag가 아니거나 모순
// 조합이면 InvalidArgument다(WireSlot.h WireValueShape).
Result<WireValueShape> ResolveSlotShape(WireSlot slot, uint32 wire, WireValueShape shape) noexcept;

// 길이 prefix 하나로 크기가 정해지는 가변 wire의 길이 prefix 위치와 고정 부분 크기다(String·Bytes는
// 0·8, SoftObjectReference는 Guid 뒤 16·24). 그 밖의 wire면 false다.
bool LengthPrefix(uint32 wire, usize& offset, usize& fixedBytes) noexcept;

// value(값 하나의 byte — exact shape로 이미 크기를 맞춘 것)의 길이 prefix가 resolved 크기를
// 만드는지 본다(가변 wire의 exact 검사). 길이 prefix가 없는 wire나 exact가 아니면 늘 참이다.
bool LengthPrefixMatches(uint32 wire, WireValueShape resolved, const uint8* value) noexcept;

// 원소 열 payload(값 하나의 ArchiveReader, 위치 0)의 header(열마다 u32 tag, 그다음 u32 count)를
// 읽고 검사한다. 검사 순서(모두 할당 전): 열의 기대 tag·shape가 그 자리의 것이 아니거나 모순이거나
// 열들의 최소 크기 합이 usize를 넘으면 InvalidArgument(이때까지 payload는 옮기지 않아 호출자
// reader가 그대로다) → payload를 옮긴다 → 저장 tag(들)가 그 자리의 tag가 아니면 CorruptData →
// 저장 tag가 기대 tag와 다르면 TypeMismatch → count > maxCount면 CapacityExceeded → count × (열마다
// max(wire 최소, shape 최소)의 합)이 남은 byte보다 크면 CorruptData(모든 열이 고정 폭이거나
// exact면 남은 byte가 정확히 그 값이 아닐 때도) → count만큼 work를 청구(한도면 CapacityExceeded) →
// 모든 열이 exact이고 길이 prefix가 있는 열이 있으면 원소마다 그 길이 prefix가 크기를 만드는지
// 본다(CorruptData — 할당하지 않는다. 모두 고정 폭이면 훑지 않는다).
Result<WireSequence> OpenWireSequence(ArchiveReader&& payload, Span<const WireColumn> columns,
                                      uint32 maxCount) noexcept;

// 원소 열 header를 out에 쓰고 원소 하나(열들을 합친 것)의 크기를 돌려준다. 열의 tag·shape·크기 합
// 검사는 reader와 같고(InvalidArgument), count > maxCount나 header + count × 원소 최소 크기 >
// maxBytes면 쓰기 전에 CapacityExceeded다. out의 쓰기 오류를 그대로 돌려준다.
Result<WireValueShape> BeginWireSequence(ArchiveWriter& out, Span<const WireColumn> columns,
                                         uint32 count, usize maxBytes, uint32 maxCount) noexcept;

// start부터 쓴 payload를 본다: out이 start보다 짧아졌으면 InvalidArgument(호출자가 out을 줄였다),
// maxBytes를 넘으면 CapacityExceeded다.
Result<void> CheckWireSequence(const ArchiveWriter& out, usize start, usize maxBytes) noexcept;

// start부터 쓴 payload를 검사한다: out이 start보다 짧아졌으면 InvalidArgument, maxBytes를 넘으면
// CapacityExceeded, header 뒤 원소 byte가 count × 원소 최소 크기보다 작거나 entry가 exact인데
// 정확히 같지 않으면 InvalidArgument(원소 수를 잘못 쓴 호출자 오류)다.
Result<void> EndWireSequence(const ArchiveWriter& out, usize start, usize headerBytes, uint32 count,
                             WireValueShape entry, usize maxBytes) noexcept;

// 원소 열 header의 byte 수다(열마다 u32 tag + u32 count).
constexpr usize WireSequenceHeaderBytes(usize columns) noexcept {
    return 4 * columns + 4;
}

} // namespace rocks::detail
