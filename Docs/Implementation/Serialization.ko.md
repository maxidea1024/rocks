# Rocks 직렬화 구현 계약

기준일: 2026-11-10. Core의 binary archive와 GUID 기반 record, ObjectRuntime의 단일 strong graph 저장·복원을 구현하였다. 구현 대상은 아래 API와 시험 fixture로 한정한다. 이번 변경의 최종 Windows/macOS 실행 결과는 [5차 검증 기록](../Reviews/ImplementationRound5.FoundationValidation.ko.md)에 기록하였다. 이전 Foundation 실행 횟수를 새 codec 검증 결과로 사용하지 않는다.

## 모듈과 공개 API

| 모듈 | 직접 의존성 | 공개 기능 |
|---|---|---|
| `rocks_serialization` | Core | `ArchiveWriter`, `ArchiveReader`, `RecordBuilder`, `RecordReader` |
| `rocks_object_serialization` | ObjectRuntime, Serialization | `SaveObjectGraph`, `LoadObjectGraph`, limits/load options/migration callback |

저장소의 공개 헤더는 `Core/Source/Runtime/Serialization/Public/Serialization`과 `Core/Source/Runtime/ObjectSerialization/Public/ObjectSerialization`에 위치한다. C++23, 명시 Allocator, move-only Result/소유 컨테이너를 사용한다. 엔진 소유 구현에는 STL 컨테이너·문자열·알고리즘과 host 구조체 메모리 덤프를 사용하지 않는다.

## Binary Archive 값 형식

| 값 | wire 형식 | 검증 |
|---|---|---|
| bool | byte 0 또는 1 | 그 밖의 값 InvalidArgument |
| u32/u64 | 각각 4/8-byte little-endian | 경계·작업 한도 |
| i64 | 8-byte two's complement little-endian | signed 극값 복원 |
| f32/f64 | IEEE 표현 bits를 little-endian 정수로 기록 | 폭·표현 컴파일 검사, NaN/음의 0 보존 시험 |
| Guid | high u64, low u64 순서 | host padding 미사용 |
| bytes | u64 byte 길이 + payload | 길이·overflow·원본 경계 |
| UTF-8 String | u64 byte 길이 + UTF-8 payload | overlong·surrogate·잘린 scalar·범위 초과 거부 |
| raw bytes | 명시 길이만큼 payload | 자동 prefix 없음 |

String의 내부 NUL은 U+0000으로 보존하며 파일에 별도 종단 NUL은 추가하지 않는다. 소유 String 복원 시 Core String의 종단 NUL을 다시 생성한다. 값의 길이는 code point/grapheme 수가 아닌 byte 수다.

`ArchiveWriter::TakeBytes`는 buffer 소유권만 이전한다. `ArchiveReader`와 반환 view는 원본의 불변 byte 범위를 빌리며 `ReadString(Allocator&)`는 독립 String을 소유한다. writer의 자기 View와 부분 View append는 재할당을 포함하여 지원한다. 입력 pointer/count가 실제 readable 범위인지 확인하는 책임은 호출자에게 있다.

## Record version 1

44-byte envelope는 magic `ROCKSREC` 8 bytes, envelope version u32=1, schema Guid 16 bytes, schema version u32, field count u32, payload byte count u64로 구성한다. 필드 TLV의 고정 부분은 field GUID 16 bytes, wire tag u32, encoded payload 길이 u64의 28 bytes다.

`RecordBuilder`는 field 추가 순서를 유지한다. GUID 정렬 canonicalization은 제공하지 않으며 동일한 추가 순서와 값에 대한 byte 재현성을 제공한다. `Open`은 전체 envelope·길이·field count·모든 TLV 경계를 검증하고, id 순으로 정렬한 index의 이웃 비교로 중복 GUID를 검사한 뒤(O(n log n), 정렬 비교 상한 2·n·bitwidth(n)+n을 work로 미리 청구) 소유 index를 반환한다. `Find`는 같은 index로 이진 탐색한다(work bitwidth(n)+1). `Next`는 기록 순서다. index 소유권으로 원본 payload 수명이 연장되지는 않는다.

unknown nonzero tag는 FieldView로 반환하여 skip할 수 있다. known tag의 의미는 상위 소비자가 기대 tag와 실제 값 표현을 확인하고, `RecordReader::OpenValue(field)`로 연 ArchiveReader로 읽어 `ArchiveReader::AtEnd`로 trailing bytes까지 검증한다. id/tag 0은 InvalidArgument, 중복 ID는 AlreadyExists, 허용되지 않는 envelope/schema version은 Unsupported, 기대 schema GUID 불일치는 TypeMismatch다. checksum/인증이 없으므로 범위 안 payload bit 변경이 반드시 오류로 검출되는 포맷은 아니다.

중첩 Record(wire tag 10 — 리플렉션 struct 값)는 `RecordReader::OpenRecordField(allocator, field, schema, minVersion, maxVersion, maxFields)`로 연다. 바깥 reader가 돌려준 필드여야 하고(범위 밖 InvalidArgument, wire가 Record가 아니면 TypeMismatch) 같은 트리 예산을 쓰되, 바깥 Open이 payload 바이트를 이미 청구했으므로 header·TLV 색인·정렬 비용만 청구한다(중첩 깊이만큼 작업이 불어나지 않는다). schema version 0은 어느 Open에서든 InvalidArgument(범위 밖 Unsupported와 구분)다. `RecordWriter`는 바깥 writer 위에 Record를 제자리로 쓴다: `Begin`이 header 자리를, `BeginField`가 TLV header 자리를 쓰고, 값은 바깥 writer에 바로 쓴 뒤 `EndField(wire)`가 wire·길이를, `End`가 필드 수·payload 크기를 채운다. 값 byte를 한 번만 쓰므로 중첩 깊이만큼 복사가 늘지 않고 같은 필드·순서면 `RecordBuilder::Build`와 같은 byte다. 필드 ID 중복은 호출자가 보증하고(스키마 필드), 실패하면 바깥 writer 내용은 정해지지 않으므로 버린다.

### Record 트리 예산

`RecordReader::Open`은 Record 트리의 누적 예산 `ArchiveBudget`(maxOperations·maxWork, 원자적 누적·포화)을 `SharedPtr`로 만든다. `OpenValue`로 연 값 reader, `RecordListReader::OpenField`로 연 중첩 목록, 그 `OpenItem` 항목 reader, `ValueArrayReader::OpenField`로 연 배열이 같은 예산을 공유하므로 값·항목마다 새 예산을 받아 한도를 우회할 수 없다. 실패한 `OpenValue`·`OpenField`·`OpenItem` 호출도 operation 하나를 소비한다. 한도를 예산이 들고 있어 공유자마다 판정이 다르지 않고, 서로 다른 reader를 여러 스레드에서 써도 사용량이 정확히 합산된다.

### RecordList (wire tag 11)

payload는 u32 count + count × (u64 length + 중첩 Record)이며 빈 목록도 count 0으로 기록한다. `RecordListLimits`는 maxItems·maxPayloadBytes(기본 4 MiB)·maxItemBytes(기본 4 MiB − 12)로, 기본값은 기본 maxValueBytes에 맞춰 기본 한도의 목록이 기본 한도의 부모 값 하나에 들어간다. builder는 완성된 payload를 기하 성장 buffer에 쌓고(실패 원자적) Finish가 부모에 한 번 복사한다. reader는 count를 할당 전에 남은 byte / 52와 비교하고, count만큼 work를 항목 파싱 전에 청구한 뒤 모든 length·항목 envelope를 검증하고 index를 게시한다. 목록 archive의 크기 한도는 목록 한도에서 오고 항목 Record 한도와 분리된다. 항목 byte는 한 번만 청구한다: 단독 `Open`은 목록을 열 때, `OpenField`는 부모 Record가 이미 청구했으므로 항목마다 operation 하나(색인)만, `OpenItem`은 언제나 항목의 header·TLV 색인만 청구한다(목록 안 struct 안 목록도 작업이 깊이 배수로 불어나지 않는다). `OpenItem(index, maxFields)`는 필드 수 한도를 호출마다 정하고(그래프의 남은 누계), `ItemBytes(index)`는 항목 Record 전체 byte(마이그레이션 입력)를 빌린 view로 준다. `RecordListWriter`는 바깥 writer 위에 목록을 제자리로 쓴다: `Begin(out, count, limits)`가 최종 항목 수를 쓰고(4 + count × 52 > maxPayloadBytes·count > maxItems면 CapacityExceeded), `BeginItem`이 u64 길이 자리만, 항목 Record는 호출자가 `RecordWriter`로 쓰고 `EndItem`이 길이를 채우며(out이 길이 자리보다 짧거나 Record envelope가 아니면 InvalidArgument, maxItemBytes 초과나 지금까지의 payload > maxPayloadBytes는 CapacityExceeded), `End`가 항목 수 == count와 전체 크기를 확인한다. 같은 항목·순서면 `RecordListBuilder::Finish`의 payload와 같은 byte다.

### SoftObjectReference (wire tag 12)

payload는 Guid 16 bytes(에셋 ID) + String(객체 경로, u64 길이 + UTF-8)이다. 형식만 정의하며 null 규칙과 경로 상한은 ObjectRuntime `SoftObjectRef`와 ObjectSerialization이 검사한다(아래 Object Graph).

### ValueArray (wire tag 13)

단일 값 원소 배열(`Serialization/ValueArray.h`)이다. payload는 u32 원소 wire tag + u32 count + count개의 원소 payload(그 wire tag의 Archive 표현 — Bool 1, U32·F32 4, U64·I64·F64 8, Guid 16, String·Bytes u64 길이 + byte, SoftObjectReference Guid + String)다. Record·RecordList·ValueArray와 모르는 tag는 원소가 될 수 없다. `ValueArrayReader::Open(ArchiveReader&&, elementWire, maxCount, shape)`·`OpenField(parent, field, …)`는 할당 전에 기대 tag 인자·shape(원소 tag가 아니거나 모순 조합 — 고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기 — 이면 가장 먼저 InvalidArgument) → 저장 tag(원소가 될 수 없으면 InvalidArgument) → 기대 tag(다르면 TypeMismatch) → count > maxCount(CapacityExceeded) → count × max(wire 최소, shape 최소) ≤ 남은 byte(고정 폭·`shape.exact`면 정확히 같아야 함, 아니면 InvalidArgument) 순으로 검사하고 count만큼 work를 청구한다. 가변 wire(String·Bytes·SoftObjectReference)에 `shape.exact`면 원소마다 길이 prefix가 정확히 그 크기를 만드는지도 본다(합계만 맞는 Digest 길이 0·64는 InvalidArgument). 원소는 `Elements()`의 `Read*`로 읽고 `Finish()`가 trailing byte를 거부한다. 이동된 reader는 원소 0개다. `ValueArrayWriter::Begin(out, elementWire, count, maxBytes, maxCount, shape)`·`Check()`·`End()`는 header에 최종 count를 쓰고 원소를 out에 바로 쓰게 한다. Begin은 reader와 같은 tag·shape 검사(InvalidArgument), count > maxCount와 8 + count × 최소 크기 > maxBytes(CapacityExceeded)를 쓰기 전에 하고, End는 전체 > maxBytes면 CapacityExceeded, 원소 byte가 count × 최소 크기보다 작거나 고정 폭·exact인데 정확히 같지 않으면 InvalidArgument다(가변 원소는 합계의 하한만 본다). 원소 tag 인자는 `uint32`와 `WireType` 오버로드가 있다. 리플렉션 배열 필드(B5b)가 이 형식을 쓴다.

## Object Graph version 1

외곽 envelope는 magic `ROCKSGRF` 8 bytes, format version u32=1, object count u32, root local ID u32=1의 20 bytes다. 각 객체는 local ID u64, TypeId Guid, schema version u32, 길이 prefix가 있는 Record bytes로 구성한다. local ID는 1부터 count까지 연속 순서다.

객체 주소나 runtime ObjectHandle의 registry/slot/generation을 저장하지 않는다. ObjectRef는 파일 내부 u64 ID로 표현하며 0은 null이다. metadata 순서 BFS로 객체 ID를 부여하여 cycle·shared identity를 보존한다. 다른 Registry에 복원한 동일 그래프를 다시 저장하는 byte 재현성을 시험한다.

지원 ValueKind는 bool, UInt8/16/32/64, Int32/64, Float32/64, Guid, Utf8String, ObjectReference, AssetId, Digest256, SoftObjectReference, Enum, Struct다. Enum은 I64 wire이며 저장(C++에서 직접 넣은 값)은 선언되지 않은 값(Flags enum은 선언 값의 OR가 아닌 값)을 InvalidArgument로 거부하고, 복원은 기본 정책에서 객체 생성 전에 거부한다(`ReportAndDefault`면 진단 후 대체 값). 값 검사 순서는 wire 종류(TypeMismatch) → 정수 폭(Overflow) → 그 밖의 값과 trailing byte(InvalidArgument)다. UInt8/16/32는 U32 wire, Int32/64는 I64 wire로 저장하고 대상 폭을 넘는 값은 Overflow로 거부한다. AssetId는 Guid wire, Digest256은 Bytes wire(길이가 정확히 32가 아니면 InvalidArgument), SoftObjectReference는 SoftObjectReference wire다. 소프트 참조는 저장 때(C++에서 직접 대입한 값)와 로드 때 모두 null 규칙(asset 0에 경로가 있으면 안 됨)과 4 KiB 경로 상한을 검사하고 어기면 InvalidArgument다. 소프트 참조는 GC 간선이 아니므로 대상 객체를 그래프에 포함하거나 찾지 않는다. Struct·Array는 B4·B5 전까지 Unsupported다. 모든 strong graph 필드가 `completeFieldLayout=true` metadata에 포함되고 schema와 필드 codec(`RuntimeFieldOps::encode`·`decode`)이 stable GUID별 1:1이어야 한다. 이 완전성 선언에는 상속 필드도 포함하며, 등록이 Transient가 아닌 모든 필드에 codec을 요구한다. Opaque·부분 metadata·누락 codec은 Unsupported다.

### 리플렉션 struct 값

struct 필드(`ValueKind::Struct`)의 값은 struct ID·버전 헤더의 중첩 Record다(설계 4.4·4.6절). 멤버는 struct 필드 ID TLV이며 struct 안 멤버도 같은 codec·정책을 따른다: 유효 플래그 전파(바깥 EditorOnly·Transient·ReadOnly), 제외 플래그, 모르는 멤버(진단 path는 감싼 struct 필드까지 — Reject 정책이면 InvalidArgument), 누락(허용되지 않으면 NotFound, 허용된 누락 struct 필드는 그 안의 Required 후손마다 전체 path로 `RequiredMissing` 진단), 저장된 Transient(Unsupported), 미선언 enum. 중첩 TLV도 그래프 필드 누계(`maxTotalFields`)에 센다.

- 버전: 같으면 그대로, 낮으면 마이그레이션 — `ObjectGraphLoadOptions.migrateStruct`(override, Unsupported면 다음)를 먼저, 그다음 타입이 등록한 `RuntimeStructOps.migrate`, 둘 다 없으면 Unsupported. 결과는 정확히 현재 버전이어야 하고(아니면 Unsupported) 다시 마이그레이션하지 않는다. 1단계가 결과를 노드에 발생 순서로 보관하고(`maxMigrationBytes` 누계, 원본 + 결과 바이트 작업) 2단계는 같은 순서의 보관본을 (struct ID, 원본 버전)으로 대조해 쓴다 — 콜백은 등장마다 한 번이다. 높으면 `newerVersion` 정책(`LoadNewerSkippingUnknown`이면 path 있는 `NewerVersion` 진단 뒤 현재 스키마로 읽는다).
- 복원 문맥은 Record 스택(`RecordScope`)을 쓴다: struct 값 안의 멤버 값은 맨 위 Record(마이그레이션 결과 포함)로 연다. 2단계는 같은 버전 범위·크기 한도에 무제한 operation·work로 다시 연다.
- struct hook(`FieldCodecHooks`): `ObservedStructs`(로드 시작 때 검사 — 미등록 NotFound, 참조 struct InvalidArgument)의 struct는 1단계에서 검증과 같은 한 번의 해석으로 채운 임시 값으로 `ObserveStruct`를 받는다(실패하면 생성 전 로드 실패). 저장은 참조 없는 struct마다 `SubstituteStruct`로 대신 쓸 값을 받는다(그 struct encode 동안 유효, null은 InvalidArgument).
- 참조 struct(강한 참조 멤버가 있는 struct)의 참조는 2단계에서 소유 객체 안 주소로 `SetReference`한다. 생성 Trace가 struct 안 참조를 방문한다.

### 필드 codec과 문맥

필드 값은 등록된 codec thunk가 쓰고 읽는다(설계: [ReflectionFoundations](../Design/ReflectionFoundations.ko.md) 6.4절). 생성 코드는 필드마다 `detail::EncodeGeneratedField`·`DecodeGeneratedField`를 쓰고, 수동 함수표는 `detail::EncodeThroughFieldValue`·`DecodeThroughFieldValue`(같은 표의 read·writeStaged)를 쓴다. 값 쓰기(`EncodeFieldWire`)와 해석·검증(`ParseFieldValue`)은 한 곳에 있다. 직렬화기는 문맥(`FieldEncodeContext`·`FieldDecodeContext`, `Object/FieldCodec.h`)을 구현하고 필드마다 `FieldScope`로 위치(필드·선언 타입·유효 플래그·enum 스키마)를 정한다. 강한 참조의 wire(graph v1 = U64 local ID)와 대상 타입 검사, 진단·소프트 참조 보고, 예산은 문맥이 맡으므로 그래프 형식이 바뀌어도 생성 코드는 그대로다. 생성 codec은 스키마의 값 형식이 정적 멤버 형식과 다르면 TypeMismatch이고(별명·수동 스키마의 오인코딩 방지), 직렬화기는 codec이 돌려준 wire가 스키마 값 종류의 것인지 다시 확인한다. 저장은 멤버를 직접 읽어 쓰며(String·경로를 복사하지 않는다) 쓴 뒤 소유자의 논리 수명을 다시 확인한다.

저장 옵션(`ObjectGraphSaveOptions`)은 limits, `FieldEncodePolicy`(`rejectMissingRequired` — Required 참조 null을 `RequiredMissing` 진단 뒤 InvalidArgument, `excludedFlags` — 그 플래그의 필드는 저장하지 않음, Transient는 항상), `FieldCodecHooks*`다. 복원 옵션(`ObjectGraphLoadOptions`)은 limits, `FieldDecodePolicy`(미선언 enum `Reject`·`ReportAndDefault`, 높은 Record 버전 `Reject`·`LoadNewerSkippingUnknown`, 모르는 필드 `SkipAndReport`·`Reject`, `allowMissingFields`, `allowMissingFlags`), migration, hooks다. `ReportAndDefault`면 일반 enum은 값 0(선언되어 있으면) 또는 첫 열거자로, Flags enum은 선언되지 않은 비트만 지운 값으로 바꾸고 `UndeclaredEnumValue`를 보고한다. 저장 버전이 현재보다 높으면 migration을 부르지 않는다 — `Reject`면 Unsupported, `LoadNewerSkippingUnknown`이면 아는 필드만 읽고 `NewerVersion`을 보고한다. 값을 바꾸거나 모르는 내용을 건너뛰는 이 두 정책은 hooks가 있어야 한다(없으면 InvalidArgument). hooks(`Report`·`CollectSoftReference`)가 실패를 돌려주면 그 저장·복원이 실패하며, 실패한 호출에서 받은 진단·수집은 무효다. 진단은 객체 타입·그래프 안 위치·필드 경로를 싣는다.

## 저장·복원 수명과 실패 규약

Save는 owner thread에서 root Borrow를 유지하여 GC 진행을 차단한다. TypeLease는 각 getter와 schema의 코드·데이터 수명을 보존한다. caller와 allocator/accessor callback도 저장 중 source graph를 변경하지 않아야 한다. 최종 output 할당 이후 root Get을 다시 검증한 다음 Array를 반환한다. 생성 codec은 멤버를 복사하지 않고 직접 쓴다(ArchiveWriter가 성장 전에 크기 한도를 검사한다). 범용 codec(read 경유)의 String 복사와 값 임시 저장에는 크기 한도·예산을 지나는 할당기(`BudgetAllocator`)를 적용하며 이 할당기를 빌리는 값은 함수 내부에서 먼저 파괴한다.

Load는 두 단계로 수행한다. 그래프 예산은 `ArchiveBudget` 하나를 모든 Record 트리(값 reader 포함)와 공유하므로 reader 작업과 그래프 청구가 같은 누계다.

1. 외곽 전체 길이와 version, ID 순서, 객체 수를 검증한다.
2. (1단계, 노드마다) 타입 lease를 확보하고 타입 기반 생성 거부(Abstract·construct 없음)와 완전한 codec layout을 검증한다. 저장 버전이 현재보다 낮으면 명시 migration callback을 호출하고(반환 record도 다시 검증), 높으면 `newerVersion` 정책을 따른다. Record 필드를 기록 순서로 돌며 모르는 필드는 정책대로 진단·건너뛰기 또는 거부, Transient 필드는 Unsupported, 아는 필드는 `decode(nullptr, …)`로 검증만 하고 (스키마 위치, 함수표, FieldView)를 보관한다. 강한 참조 token은 범위를 검사해 기대 형식과 함께 보관한다. decode 직후 2단계 몫(같은 reader 작업량, staged 쓰기·문자열 복사·참조 연결)을 미리 청구한다. Record에 없는 영속 필드는 누락이 허용되지 않으면 NotFound, 허용된 Required 누락은 `RequiredMissing`을 보고한다.
3. 보관한 token으로 대상 native type(스키마 기대 타입과 정적 thunk 대상 타입 모두, (기대, 실제) 쌍 캐시)과 root에서의 전체 도달성을 검사한다. dangling ID와 파일의 고아 객체를 거부한다. 노드 생성·Commit·staged 간선 몫을 청구한다.
4. (2단계) ConstructionContext에서 모든 객체를 먼저 생성하고 보관한 FieldView로 `decode(&transaction, …)`를 불러 값을 쓴 뒤 첫 root를 한번에 Commit한다. 2단계는 그래프 예산에 접근하지 않으며(값 reader는 같은 크기 한도·무제한 work) 입력 내용 때문에 실패하지 않는다 — 실패는 할당과 Registry 상태뿐이다. 진단·소프트 수집은 1단계에서 한 번만 한다.

어떤 실패에서도 부분 객체를 외부에 공개하지 않는다. 생성 단계 실패는 역순 rollback한다. allocator의 누적 통계와 재사용 slot generation이 이전 숫자로 복원된다는 의미는 아니다. 전달 TypeRegistry가 대상 ObjectRegistry의 실제 저장소와 다르면 WrongContext다. 강한 참조의 대상은 필드 기대 타입(`value.type`)과 생성 thunk의 정적 대상 타입 각각의 native 타입이거나 그 파생이어야 하며(TypeId가 달라도 native가 같으면 허용), 1단계가 객체를 만들기 전에 이것을 검사한다(staged setter와 같은 상속 규칙).

unknown field는 기본 진단 후 skip한다(`UnknownFieldPolicy::Reject`면 InvalidArgument). 누락 known field는 기본 NotFound이며 `policy.allowMissingFields` 또는 그 필드 플래그가 `policy.allowMissingFlags`에 있을 때만 native 기본값을 유지한다. unknown field를 제외한 결과 일부 파일 객체가 고아가 되면 도달성 검증에서 거부한다. schema version 변경은 callback 없이 자동 허용하지 않는다.

## 크기·처리 한도

| 항목 | 기본값 | 적용 범위 |
|---|---:|---|
| Archive 전체 bytes | 16MiB | 입력/출력 buffer 길이 |
| Archive 가변 값 bytes | 4MiB | bytes/string/raw, 고정 폭 primitive 제외 |
| Archive operation | 1,048,576 | 실패를 포함한 API 호출 |
| Archive work | 64Mi 단위 | byte 검사·복사·탐색 비용 |
| Record fields | 4,096 | 레코드별 TLV 수 |
| Graph objects | 1,024 | 단일 root 그래프 객체 수 |
| Graph total fields | 1,048,576 | 원본·migration 재검증·중첩 struct TLV의 누계(Record 하나의 필드 수 한도도 남은 누계) |
| Graph bytes/value bytes | 16MiB/4MiB | 전체 파일/개별 encoded field |
| Graph migration bytes | 16MiB | callback 반환 buffer 누계 |
| Graph work | 64Mi 단위 | lookup, codec 처리, index 초기화, fixup 비용 |

Archive의 read 실패는 cursor를, write 실패는 기존 bytes와 길이를 보존한다. 실패에 소비한 operation/work는 환급하지 않는다. 모든 size 합·곱과 비용 누계는 overflow 검사 또는 잔량 비교를 선행한다. 무제한 재시도에 의한 예산 우회는 허용하지 않는다.

maxBytes는 전체 RAM peak 제한과 같지 않다. 저장 시 중간 record와 최종 output이, 복원 시 원본·Record 색인·필드 색인·참조 token·migration output·staged 객체가 함께 존재할 수 있다. native 객체 크기는 등록 타입에 의존한다. 전체 메모리 quota는 allocator 정책으로 추가 적용한다. migration/accessor/생성자 등 신뢰된 사용자 코드의 임의 CPU·전역 할당을 codec이 강제 중단하지 않는다.

## 검증 구성과 후속 범위

Archive/Record 시험에는 독립 little-endian golden, IEEE bits, UTF-8/NUL, 비정상 길이·bool·version·tag, compound cursor 불변, 자기 alias 성장, 모든 할당 지점 N-fail, 실패 예산 유지, field iteration/move 수명, 결정적 byte mutation, 최대 필드 수(4096)의 정렬 index·이진 탐색 비용과 비이웃 중복이 포함된다. RecordList 시험에는 손으로 조립한 payload와의 byte 비교, 잘림·length 불일치·count 사전 검사, 모든 한도의 정확한 경계(builder·reader), 기본 한도 조합(4 MiB − 12 항목), 트리 예산 공유·포화·스레드 합산, 실패 호출의 operation 소비, 부모 한도 전파, 다항목 N-fail 후 재시도 결과의 byte 동일성과 누수 검사가 포함된다. ObjectGraph 시험에는 cycle/shared identity, 다른 Registry 복원, scalar/String/Guid, AssetId·digest·소프트 참조(왕복·재저장 byte 동일성·경로 4 KiB 경계·직접 대입한 잘못된 값의 저장 거부·wire 종류·길이·잘림·trailing 오류 행렬·save/load N-fail), enum(왕복·재저장 byte 동일성·선언되지 않은 값의 저장·복원 거부·잘못된 wire), unknown/missing policy, migration, 참조·폭·trailing 오류, TypeRegistry 불일치, incomplete metadata, save/load 전체 N-fail과 객체/할당/lease 원상태 검사가 포함된다.

소프트 참조의 해석·로드(값 저장·복원만 지원), 다중 root, 임의 컨테이너와 native 숨은 필드, 비동기·병행 graph snapshot, 파일 IO, 압축·암호화, 무결성 인증은 현재 구현 범위에 포함하지 않는다. 장기 계획의 전체 serialization/cook/savegame 완료로 합산하지 않는다. 지원하지 않는 표현을 raw struct dump나 성공한 부분 저장으로 대체하지 않는다.

## 참조 타입 사전 검증

강한 참조(ObjectReference)에만 해당한다. 소프트 참조의 `value.type`(기대 타입)은 저장·복원에서 검사하지 않는다(대상을 찾을 때 검사한다). nonnull 참조의 `value.type`이 0이 아니면 해당 등록 lease의 native type과 실제 대상의 native type을 비교한다. 파생 타입의 `IsA`만으로 참조를 허용하지 않으며 모든 생성자 실행 전에 TypeMismatch로 거부한다. 동일 native type을 다른 TypeId로 등록한 별명은 허용하여 기존 Registry setter 규약과 일치시킨다. `value.type=0`은 기반 Object의 일반 참조다. nonnull 참조의 명시 대상 타입이 등록되지 않았으면 NotFound를 반환한다. null 참조는 대상 등록을 요구하지 않는다.
