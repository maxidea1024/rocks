#pragma once

#include <Serialization/WireSlot.h>

// 키·값 맵 필드(WireType::Map = 16)다(설계: Docs/Design/ReflectionExtensions.ko.md 1.2·7.2·7.4절 —
// 리플렉션 OrderedMap<K, V>·ObjectRefMap<K, T>).
//
// 형식: payload = u32 키 wire tag + u32 값 wire tag + u32 count + count × (키 payload + 값
// payload)(삽입 순서). 키 tag는 WireSlot::Key, 값 tag는 WireSlot::MapValue 자리의 것이다. 값이
// Record(값 struct)면 u64 길이 + Record(PrefixedRecordWriter·ReadPrefixedRecord), InstancedStruct면
// u64 길이 + 내용(InstancedStructWire.h), Text면 형태 byte + 내용(TextWire.h)이고 그 밖은 그
// wire의 Archive 표현이다. 빈 맵도 count 0으로 기록한다.
//
// 이 계층은 형식(tag·count·길이·잘림)만 본다. 키 중복은 codec 1단계가 종류 규칙으로
// 판정한다(String은 byte, Name은 Core Name 규칙 — 중복은 InvalidArgument).
//
// 사용 예(쓰기 — Map<String, struct>):
//   auto map = MapWireWriter::Begin(out, stringWire, recordWire, count, maxBytes);
//   if (!map) {
//       return Result<void>::Failure(map.Error());
//   }
//
//   for (...) {
//       if (auto key = out.WriteString(name); !key) {
//           return key;
//       }
//
//       auto value = PrefixedRecordWriter::Begin(out, recordWire);
//       // ... RecordWriter로 값 Record를 out에 쓴다
//       if (auto ended = value.Value().End(); !ended) {
//           return ended;
//       }
//   }
//
//   return map.Value().End();
//
// 사용 예(읽기 — 부모 Record 트리와 같은 예산):
//   auto map = MapWireReader::OpenField(parent, field, stringWire, recordWire, maxCount);
//   if (!map) {
//       return Result<void>::Failure(map.Error());
//   }
//
//   for (uint32 index = 0; index < map.Value().Count(); ++index) {
//       auto key = map.Value().Entries().ReadStringView();
//       auto value = ReadPrefixedRecord(map.Value().Entries());
//       // 실패를 확인하고 값 Record는 parent.OpenNestedRecord로 연다
//   }
//
//   return map.Value().Finish(); // 남은 byte는 InvalidArgument
//
// 오류는 ValueArray와 같다: 잘림·받지 않는 tag·크기 불일치·남은 byte는 InvalidArgument, 기대 tag와
// 다른 저장 tag는 TypeMismatch, 개수·크기 한도 초과는 CapacityExceeded다.
namespace rocks {

// Map payload를 header부터 검증하고 항목(키, 값)을 차례로 읽게 한다. 열 때 할당하지 않고 항목
// 수만큼의 work를 먼저 청구한다. 넘겨받은 ArchiveReader의 예산을 그대로 쓴다. 원본 byte는
// reader보다 오래 살아야 한다. 이동만 되며 이동된 원본은 항목 0개의 빈 reader다.
class MapWireReader {
public:
    // payload(값 하나의 ArchiveReader, 위치 0)의 header를 검사한다. 검사 순서(모두 할당 전):
    // keyWire·keyShape가 키 자리의 것이 아니거나 valueWire·valueShape가 값 자리의 것이 아니거나
    // 모순이거나 키·값 최소 크기 합이 usize를 넘으면 InvalidArgument(이때는 payload를 옮기지
    // 않는다) → 저장 키 tag·값 tag가 그 자리의 tag가 아니면 InvalidArgument → 저장 키 tag ≠ keyWire
    // 또는 저장 값 tag ≠ valueWire면 TypeMismatch → count > maxCount면 CapacityExceeded → count ×
    // (키 최소 크기 + 값 최소 크기)가 남은 byte보다 크면 InvalidArgument(키·값이 모두 고정 폭이거나
    // exact면 남은 byte가 정확히 그 값이 아닐 때도) → count만큼 work를 청구(한도면
    // CapacityExceeded) → 키·값이 모두 exact면 항목마다 가변 칸의 길이 prefix가 그 크기를 만드는지
    // 본다(InvalidArgument). 한쪽이 exact가 아닌 가변이면 항목 간격이 정해지지 않으므로 합계 하한만
    // 보고, 원소 경계·exact 길이는 항목 Read와 호출자가 본다(예: Digest 키는 ReadBytes 뒤 길이 32
    // 확인).
    static Result<MapWireReader> Open(ArchiveReader&& payload, uint32 keyWire, uint32 valueWire,
                                      uint32 maxCount, WireValueShape keyShape = {},
                                      WireValueShape valueShape = {}) noexcept;

    static Result<MapWireReader> Open(ArchiveReader&& payload, WireType keyWire, WireType valueWire,
                                      uint32 maxCount, WireValueShape keyShape = {},
                                      WireValueShape valueShape = {}) noexcept {
        return Open(Move(payload), static_cast<uint32>(keyWire), static_cast<uint32>(valueWire),
                    maxCount, keyShape, valueShape);
    }

    // parent가 돌려준 Map 필드를 parent와 같은 예산·한도로 연다. operation 하나를 청구하며 이동된
    // parent·parent 밖의 범위는 InvalidArgument, wire tag가 Map이 아니면 TypeMismatch다. 그 뒤는
    // Open과 같다.
    static Result<MapWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                           uint32 keyWire, uint32 valueWire, uint32 maxCount,
                                           WireValueShape keyShape = {},
                                           WireValueShape valueShape = {}) noexcept;

    static Result<MapWireReader> OpenField(const RecordReader& parent, const FieldView& field,
                                           WireType keyWire, WireType valueWire, uint32 maxCount,
                                           WireValueShape keyShape = {},
                                           WireValueShape valueShape = {}) noexcept {
        return OpenField(parent, field, static_cast<uint32>(keyWire),
                         static_cast<uint32>(valueWire), maxCount, keyShape, valueShape);
    }

    MapWireReader(MapWireReader&& other) noexcept;
    MapWireReader& operator=(MapWireReader&& other) noexcept;
    MapWireReader(const MapWireReader&) = delete;
    MapWireReader& operator=(const MapWireReader&) = delete;

    // header의 항목 수다.
    uint32 Count() const noexcept;

    // 항목을 읽는 reader다(header 바로 뒤). 항목마다 키 tag의 Read*, 이어서 값 tag의 Read*를
    // 부른다(값 Record는 ReadPrefixedRecord, InstancedStruct는 ReadInstancedStructElement, Text는
    // ReadTextWire). 내용은 그 Read*가 검사한다.
    ArchiveReader& Entries() noexcept;

    // 모든 항목을 읽은 뒤 부른다. 남은 byte가 있으면 InvalidArgument다.
    Result<void> Finish() const noexcept;

private:
    MapWireReader(ArchiveReader&& archive, uint32 count) noexcept;

    ArchiveReader archive_;
    uint32 count_ = 0;
};

// 바깥 ArchiveWriter 위에 Map payload를 제자리로 쓴다. Begin이 header(키·값 tag와 최종 count)를
// 쓰고, 호출자는 항목마다 키·값을 out에 바로 쓴 뒤 End를 부른다. maxBytes는 header를 포함한 payload
// 전체의 상한이다. 작업량은 out의 청구다. 실패하면 out의 내용은 정해지지 않으므로 그 writer를
// 버린다. out은 이 writer보다 오래 살아야 한다.
class MapWireWriter {
public:
    // keyWire·valueWire가 그 자리의 tag가 아니거나 shape와 모순이면 InvalidArgument(reader와 같은
    // 검사), count > maxCount나 header 12 byte + count × (키 + 값 최소 크기) > maxBytes면 쓰기 전에
    // CapacityExceeded다. maxCount·shape는 reader에 넘길 값과 같게 준다. out의 쓰기 오류를 그대로
    // 돌려준다.
    static Result<MapWireWriter> Begin(ArchiveWriter& out, uint32 keyWire, uint32 valueWire,
                                       uint32 count, usize maxBytes,
                                       uint32 maxCount = NumericLimits<uint32>::Max(),
                                       WireValueShape keyShape = {},
                                       WireValueShape valueShape = {}) noexcept;

    static Result<MapWireWriter> Begin(ArchiveWriter& out, WireType keyWire, WireType valueWire,
                                       uint32 count, usize maxBytes,
                                       uint32 maxCount = NumericLimits<uint32>::Max(),
                                       WireValueShape keyShape = {},
                                       WireValueShape valueShape = {}) noexcept {
        return Begin(out, static_cast<uint32>(keyWire), static_cast<uint32>(valueWire), count,
                     maxBytes, maxCount, keyShape, valueShape);
    }

    // 지금까지 쓴 payload가 maxBytes 안인지 본다(넘으면 CapacityExceeded). out이 Begin 위치보다
    // 짧아졌으면(호출자가 out을 줄였다) InvalidArgument다.
    Result<void> Check() const noexcept;

    // out이 Begin 위치보다 짧아졌으면 InvalidArgument, payload가 maxBytes를 넘으면
    // CapacityExceeded, 항목 byte가 count × 항목 최소 크기보다 작거나 키·값이 모두 고정
    // 폭·exact인데 정확히 같지 않으면 InvalidArgument다.
    Result<void> End() const noexcept;

private:
    MapWireWriter(ArchiveWriter& out, usize start, uint32 count, usize maxBytes,
                  WireValueShape entry) noexcept
        : out_(&out), start_(start), count_(count), maxBytes_(maxBytes), entry_(entry) {
    }

    ArchiveWriter* out_;
    usize start_;
    uint32 count_;
    usize maxBytes_;
    WireValueShape entry_; // 키·값을 합친 항목 크기
};

} // namespace rocks
