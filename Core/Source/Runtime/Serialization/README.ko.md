# Rocks 직렬화 기반층

`rocks_serialization`은 Core만 의존하는 bounded binary archive와 stable GUID TLV record를 제공한다. STL 컨테이너나 host 구조체 메모리 덤프를 사용하지 않는다. 실제 실행 결과는 통합 빌드 검증 보고서에 별도로 기록한다.

## 값과 수명

- `ArchiveWriter`는 전달 allocator로 byte buffer를 소유한다. `TakeBytes()`는 같은 allocator를 빌리는 `Array<uint8>`로 소유권을 이전한다. allocator가 결과보다 먼저 파괴되면 안 된다.
- `ArchiveReader`는 caller의 불변 byte 범위를 빌린다. `ReadStringView`/`ReadBytes`/`ReadRaw` 결과도 원본 수명에 종속된다. `ReadString(allocator)`만 문자열을 깊게 복사한다.
- 고정 폭 값은 little-endian이다. bool은 0/1, i64는 two's complement, float32/64는 IEEE bit 표현이다. Guid는 high u64 다음 low u64다. bytes/string은 u64 byte 길이와 payload이며 종단 NUL을 파일에 쓰지 않는다. UTF-8 string의 내부 U+0000은 보존한다.
- 모든 실패는 reader cursor 또는 writer의 기존 bytes/길이를 보존한다. 소비한 operation/work 예산은 환급하지 않으므로 같은 실패를 반복하여 한도를 우회할 수 없다. OOM 재시도에도 남은 예산이 필요하다.
- maxTotalBytes는 archive 전체, maxValueBytes는 가변 값/raw 범위, maxOperations/maxWork는 호출 및 처리 비용 한도다. work는 byte 검사·복사와 탐색 비교의 보수적인 비용이며 외부 allocator 실행 시간을 제한하는 타이머가 아니다.
- 동일 wrapper의 동시 변경과 allocator callback에서 해당 wrapper/input의 변경은 지원하지 않는다. writer 자기 View의 전체/부분 append는 성장해도 지원한다.

## Record version 1

헤더는 `ROCKSREC` 8 bytes, envelope version u32, schema Guid, schema version u32, field count u32, payload byte count u64의 44 bytes다. 필드마다 GUID 16 bytes, wire tag u32, payload 길이 u64, encoded payload를 쓴다.

`RecordBuilder`는 추가 순서를 보존한다. 같은 순서의 입력은 같은 bytes를 만들지만 GUID 정렬 canonicalization은 하지 않는다. `RecordReader::Open`은 전체 envelope와 모든 필드 경계·중복 ID를 검증한 뒤 index를 반환한다. 중복 검사는 id 순으로 정렬한 index의 이웃 비교(O(n log n), 정렬 비교 상한을 work로 미리 청구)이며, 같은 index로 `Find`가 이진 탐색한다(work는 log 비용). `Next`는 기록 순서를 따른다. reader는 index만 소유하므로 caller의 원본 buffer를 유지해야 한다.

unknown nonzero wire tag는 그대로 열거하고 skip할 수 있다. 알려진 tag도 record 계층이 payload 의미를 대신 검증하지 않는다. 소비자가 기대 tag를 확인하고 `RecordReader::OpenValue(field)`로 연 ArchiveReader(Record 트리 예산을 공유한다)로 값을 읽은 다음 `AtEnd()`까지 확인한다. ID/tag 0은 InvalidArgument, 중복 ID는 AlreadyExists, envelope/schema version 미지원은 Unsupported, 기대 schema GUID 불일치는 TypeMismatch다. 누락 필드와 iteration 끝은 NotFound다.

## SoftObjectReference (wire tag 12)

`WireType::SoftObjectReference` payload는 Guid 16 bytes(에셋 ID)와 String(객체 경로, u64 길이 + UTF-8)이다. Serialization은 형식만 정의하며 null 규칙(asset 0에 경로가 있으면 안 됨)과 4 KiB 경로 상한은 ObjectRuntime `SoftObjectRef`와 ObjectSerialization이 검사한다.

## RecordList (wire tag 11)

`WireType::RecordList` payload는 u32 count와 count개의 (u64 length + 중첩 Record)다. 각 length는 중첩 Record의 전체 길이(44 + payloadBytes)와 같아야 하고, 빈 목록도 count 0으로 기록한다. `RecordListBuilder::AddItem`은 완성된 항목 Record 바이트를 깊게 복사하고(envelope magic·길이 일관성 검사), `Finish(parent, fieldId)`가 부모의 필드 하나로 추가한다. builder는 완성된 payload를 기하 성장 buffer에 바로 쌓고 Finish는 그것을 부모에 한 번 복사한다. 실패하면 builder와 부모의 필드·bytes는 호출 전 상태이며, 부모가 이미 소비한 operation·work 예산만 환급되지 않는다.

`RecordListLimits`는 maxItems(항목 수), maxPayloadBytes(목록 payload 전체, 기본 4 MiB), maxItemBytes(항목 Record 하나, 기본 4 MiB − 12)다. 기본값은 기본 `ArchiveLimits::maxValueBytes`(4 MiB)에 맞춰, 기본 한도로 만든 목록이 기본 한도의 부모 값 하나에 그대로 들어가고 다시 읽힌다(가장 큰 항목 하나짜리 목록도 count 4 + length 8을 더해 4 MiB). 부모 Record 한도는 별개로 적용되므로 큰 목록은 두 한도를 함께 올린다. builder와 reader는 이 크기·개수 한도를 같게 판정한다. 읽기의 operation·work 예산은 이와 별개로 Record 트리 전체에 누적되므로(아래 "예산 공유"), 크기 한도 안의 목록이라도 항목·필드가 아주 많은 트리는 `RecordLimits.archive`의 maxOperations·maxWork를 올려 연다.

`RecordListReader`는 count를 할당 전에 남은 byte / 52(항목 최소 크기)와 비교하고, count만큼의 work를 항목 파싱 전에 청구한 뒤, 모든 length와 항목 envelope 경계를 검증하고 index를 게시한다. 항목은 `OpenItem`이 schema·버전 범위를 검사하며 `RecordReader`로 연다(`OpenItem(index, maxFields)`는 필드 수 한도를 호출마다 정한다). `ItemBytes(index)`는 항목 Record 전체 byte(마이그레이션 입력)를 빌린 view로 돌려준다. 잘림·length 불일치·남은 바이트·잘못된 항목 envelope는 InvalidArgument, `RecordListLimits` 초과는 CapacityExceeded다.

항목 byte는 한 번만 청구한다: 단독 `Open`은 목록을 열 때 항목 byte를 청구하고, `OpenField`는 부모 Record가 이미 청구했으므로 항목마다 operation 하나(색인)만 청구한다. `OpenItem`은 언제나 항목 Record의 header·TLV 색인만 청구한다. 그래서 목록 안 struct 안 목록처럼 중첩해도 작업이 깊이 배수로 불어나지 않는다(`RecordReader::OpenRecordField`와 같은 모델).

`RecordListWriter`는 바깥 `ArchiveWriter` 위에 목록을 제자리로 쓴다(리플렉션 struct 원소 배열). `Begin(out, count, limits)`가 최종 항목 수를 쓰고(count > maxItems이거나 4 + count × 52 > maxPayloadBytes면 쓰기 전에 CapacityExceeded), 항목마다 `BeginItem`이 u64 길이 자리만 쓰며 항목 Record는 호출자가 `RecordWriter`로 out에 바로 쓴 뒤 `EndItem`이 길이를 채운다(out이 길이 자리보다 짧거나 Record envelope가 아니면 InvalidArgument, maxItemBytes 초과나 지금까지의 payload > maxPayloadBytes는 CapacityExceeded). `End`는 항목 수가 count와 같고 전체가 maxPayloadBytes 안인지 확인한다. 같은 항목을 같은 순서로 쓰면 `RecordListBuilder::Finish`의 payload와 같은 byte다.

## ValueArray (wire tag 13)

`WireType::ValueArray`는 단일 값 원소 배열이다(`Serialization/ValueArray.h`). payload는 u32 원소 wire tag, u32 count, count개의 원소 payload(그 wire tag의 Archive 표현을 이어 붙인다 — Bool 1, U32·F32 4, U64·I64·F64 8, Guid 16, String·Bytes u64 길이 + byte, SoftObjectReference Guid + String)다. Record·RecordList·ValueArray와 모르는 tag는 원소가 될 수 없다. 원소마다 Record envelope(최소 52 byte)를 쓰지 않으므로 원소 수가 많아도 값 하나의 byte 한도 안에 든다.

`ValueArrayReader::Open(ArchiveReader&&, elementWire, maxCount, shape)`·`OpenField(parent, field, …)`는 할당 전에 header를 검사한다: 기대 tag 인자가 원소 tag가 아니거나 shape와 모순이면(고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기) 먼저 InvalidArgument, 원소가 될 수 없는 저장 tag는 InvalidArgument, 기대 tag와 다르면 TypeMismatch, count > maxCount면 CapacityExceeded, count × max(wire 최소 크기, shape 최소 크기)가 남은 byte보다 크면 InvalidArgument(고정 폭이나 `shape.exact`면 남은 byte가 정확히 같아야 한다 — Digest는 `{40, true}`), 그 뒤 count만큼 work를 먼저 청구한다. 가변 wire에 `shape.exact`면 원소마다 길이 prefix가 정확히 그 크기를 만드는지도 본다(합계만 맞고 경계가 어긋난 입력을 거부한다). 원소는 `Elements()`의 `Read*`로 읽고(원소마다 operation 1 + byte, bool·UTF-8·길이는 그 `Read*`가 검사), `Finish()`가 남은 byte를 거부한다. `OpenField`로 열거나 `OpenValue`로 연 reader를 넘기면 Record 트리 예산을 공유한다. `ValueArrayWriter::Begin(out, elementWire, count, maxBytes, maxCount, shape)`는 reader와 같은 tag·shape 검사를 하고 header를 쓰며(count > maxCount나 8 + count × 최소 크기 > maxBytes면 쓰기 전에 CapacityExceeded), 호출자가 원소를 out에 바로 쓴 뒤 `End`가 전체 ≤ maxBytes와 원소 byte(고정 폭·exact는 정확히 count × 크기, 가변은 하한)를 확인한다. `Check()`는 가변 원소를 쓰는 동안 일찍 끊는 데 쓴다.

## 예산 공유(Record 트리)

`RecordReader::Open`은 그 Record 트리의 누적 예산 `ArchiveBudget`(RecordLimits.archive의 maxOperations·maxWork)을 만든다. `RecordReader::OpenValue(field)`로 연 값 reader, `RecordListReader::OpenField(parent, field, …)`로 연 중첩 목록과 그 `OpenItem` 항목 reader, `ValueArrayReader::OpenField`로 연 배열이 모두 같은 예산을 쓴다. 값·항목마다 새 예산을 주지 않으므로 위 "환급하지 않는다" 원칙이 트리 전체에 적용되어, 중첩 목록을 순회해도 한도를 넘지 못한다(`ArchiveReader::Create`로 값을 따로 읽으면 새 예산이 생기므로 신뢰하지 않는 입력은 `OpenValue`로 읽는다). 최상위 목록을 payload만으로 열 때(`RecordListReader::Open`)는 새 예산을 만든다.

예산은 `SharedPtr`로 공유되어 reader의 파괴·이동 순서에 제약이 없고, 한도를 예산이 들고 있어 공유자가 서로 다른 한도로 판정하지 않는다. 사용량은 원자적으로 누적·포화하므로 같은 예산을 공유하는 서로 다른 reader를 여러 스레드에서 써도 된다(같은 reader의 병행은 여전히 금지). work 한도를 넘으면 공유 사용량이 한도로 포화되어 형제 reader의 이후 호출도 실패한다. 실패한 `OpenItem`·`OpenValue`·`OpenField` 호출도 operation을 소비한다. 이동된 `ArchiveReader` 원본은 빈 입력이 되어 남은 자기 예산으로 같은 입력을 다시 읽지 못한다.

이 계층은 checksum/인증/압축/파일 IO/객체 생성/참조 fixup을 제공하지 않는다. 범위 안의 payload bit 변경이 반드시 오류라는 뜻은 아니며, 데이터 무결성 검증이 필요한 상위 포맷은 별도 정책을 제공해야 한다.

## 검증 구성

독립 golden endian bytes, float bit 보존, UTF-8/NUL, compound cursor 불변, self-alias 성장, 크기·예산 경계, 모든 할당 지점 N-fail, 중복·truncation·version·unknown tag, 결정적 mutation fixture를 둔다. 테스트 소스 작성과 실제 Windows/macOS 실행 증거는 구분한다.
