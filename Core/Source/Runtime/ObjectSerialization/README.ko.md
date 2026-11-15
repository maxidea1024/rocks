# Rocks 객체 그래프 저장과 복원(graph v2)

`rocks_object_serialization`은 ObjectRuntime과 Serialization을 연결한다. 공개 API는 `SaveObjectGraphSet`(호출자가 고른 저장 집합)·`SaveObjectGraph`(편의 저장 — root에서 닿는 객체)·`ObjectGraphStage`(단계형 복원 — 검증은 어느 스레드나, 생성·적용·Required 검사는 owner thread, Commit은 호출자)·`LoadObjectGraph`(Stage 위의 편의 복원)다(설계: [ObjectFoundations](../../../../Docs/Design/ObjectFoundations.ko.md) 5절). 완전한 typed metadata로 표현되는 강한 참조 그래프를 처리한다. 외부 참조(허락 hook·import 번호 재배정·Stage의 imports)는 A0-Oe다. source 코드와 시험 fixture는 구현 범위를 설명하며 실제 플랫폼별 통과 여부는 별도 통합 검증 보고서를 따른다.

## 지원 범위

bool, uint8/16/32/64, int32/64, float32/64, Guid, UTF-8 String, 강한 ObjectRef, AssetId(Guid wire), Sha256Digest(Bytes wire, 정확히 32 byte), SoftObjectRef(SoftObjectReference wire), 리플렉션 enum(I64 wire), 리플렉션 struct(Record wire의 중첩 Record — 버전·마이그레이션·struct hook은 [직렬화 구현 계약](../../../../Docs/Implementation/Serialization.ko.md) "리플렉션 struct 값"), 리플렉션 배열(`Array<T>`·null 원소를 받는 `ObjectRefArray<T>`는 ValueArray wire, struct 원소의 `Array<S>`·`StructRefArray<S>`는 RecordList wire — 같은 문서 "리플렉션 배열 값")를 지원한다. enum의 선언되지 않은 값은 저장에서 InvalidArgument이고, 복원은 기본 정책에서 거부하며 `ReportAndDefault`면 진단 후 대체 값을 쓴다. 소프트 참조는 값으로 저장하며 대상을 그래프에 넣거나 찾지 않는다. 저장(C++에서 직접 대입한 값)과 복원 모두 null 규칙(asset 0에 경로가 있으면 안 됨)과 4 KiB 경로 상한을 검사해 InvalidArgument로 거부한다. 좁은 unsigned는 U32, signed는 I64 wire 형식을 사용하고 복원 시 실제 값 폭을 검사한다. unknown GUID field는 기본 진단 후 skip한다(`UnknownFieldPolicy::Reject`면 거부). known field의 wire tag·값·trailing byte는 엄격히 검사한다.

`RuntimeTypeOps::completeFieldLayout`이 true이고 schema와 필드 codec(`RuntimeFieldOps::encode`·`decode`)이 stable field GUID별 1:1이어야 한다. 값은 등록된 codec이 쓰고 읽으며, 복원은 두 단계(1단계 검증·사전 청구, 2단계 생성·적용)이고 decode 정책·진단 hook·소프트 참조 수집은 `ObjectGraphSaveOptions`·`ObjectGraphLoadOptions`로 정한다(Docs/Implementation/Serialization.ko.md "필드 codec과 문맥"). 이 선언에는 상속 필드를 포함한 모든 영속 필드와 모든 강한 간선이 포함된다(ROCKS_NATIVE 멤버는 영속 필드가 아니고 강한 참조를 담을 수 없어 포함 대상이 아니다). 기본 수동 metadata는 false이며 부분 metadata·Opaque·누락 codec은 Unsupported다(Transient 필드는 codec이 없어도 된다). 수동 작성자가 거짓 완전성을 선언하는 임의 플러그인의 메모리 안전성을 검증하는 기능은 아니다.

객체 포인터나 ObjectHandle의 registry/slot/generation bytes는 저장하지 않는다. 노드마다 1부터 시작하는 local ID를 쓰고 강한 참조는 U32 local ID, null은 0으로 쓴다(`0x80000000 | i`는 외부 참조 i — A0-Oe). 동일 객체를 중복 생성하지 않으므로 cycle과 shared identity가 유지된다. `SaveObjectGraphSet`은 호출자가 준 순서가 local ID이고 어느 노드에서도 닿지 않는 노드(고아)도 쓴다. 집합 밖 참조는 `GraphExternalPolicy::Reject`(기본 — `ExternalReference` 진단 뒤 InvalidArgument) 또는 `Include`(발견 순으로 노드에 더하고, 그 Outer 사슬 중 집합에 없는 조상을 먼저 더한다 — 기준 Outer·노드 1의 조상·패키지 정책의 다른 패키지·기준 Outer가 있을 때 다른 트리의 객체는 `ExternalReference` 진단 뒤 InvalidArgument)로 처리한다. 편의 `SaveObjectGraph`는 root 하나와 `Include`·NodeTable로 metadata 필드 순서 BFS의 ID·출력 순서를 만든다. 원본과 다른 Registry로 복원해도 동등한 그래프를 다시 저장하면 같은 bytes가 된다.

## 포맷

header는 `ROCKSGRF` 8 bytes, formatVersion u32=2, flags u32(NodeTable 1 · BaseOuter 2 — BaseOuter는 NodeTable과만), nodeCount u32, externalCount u32의 24 bytes다. NodeTable이 있으면 노드마다 TypeId Guid, outer u32(0 = 그래프 밖 — BaseOuter면 기준 Outer, 아니면 Outer 없음 / 1..자기 번호−1 = 노드), 이름 String을 쓰고, 이어서 노드마다 `WriteBytes`로 감싼 `ROCKSREC` record를 쓴다(schema = 노드 타입, version = 저장 당시 스키마 버전 — 외곽에 버전을 따로 쓰지 않는다). 남는 byte는 없다. formatVersion이 2가 아니면 Unsupported(v1은 읽지 않는다), 정의되지 않은 flags·NodeTable 없는 BaseOuter·Record schema와 노드 타입 불일치는 InvalidArgument다. NodeTable이 없는 그래프는 복원 호출자가 노드 표를 준다(A1 패키지 Objects 섹션 — `ObjectGraphInput::nodes`). NodeTable을 쓰는 저장은 모든 노드의 Outer가 그 노드보다 앞의 노드이거나 기준 Outer(노드 1의 Outer)여야 한다(Outer 없음과 기준 Outer를 섞을 수 없다 — 어기면 InvalidArgument).

## 저장 수명

저장 전체 동안 첫 노드 Borrow를 유지하여 GC의 진행을 막고, 노드마다 encode 동안 고정(PinForEncode)하며, 각 TypeLease로 getter 코드/metadata 수명을 보존한다. caller와 allocator/accessor callback은 source graph를 변경하지 않아야 한다. 마지막 output 할당 callback 뒤 모든 노드의 생존을 재검증하고 결과를 게시한다. 결과(`ObjectGraphSaveResult` — byte와 노드 목록, 편의 API는 byte Array)는 caller allocator를 빌리며 source Registry를 참조하지 않는다(handle 값만 담는다).

String getter 복제에는 임시 bounded forwarding allocator를 사용해 큰 할당을 값 검사 전에 거부한다. 해당 FieldValue를 adapter보다 먼저 파괴하고 output에는 원 caller allocator만 보관한다. metadata/getter/allocator의 임의 외부 코드 자체를 강제 중단하는 sandbox는 아니다.

## 복원과 실패 보장

`ObjectGraphStage`는 그래프 하나를 `Validate`(1단계) → `CreateObjects`(2a) → `ApplyFields`(2b) → `CheckRequired`(2c) 순서로 한 번씩 진행하고 Commit은 호출자가 `ConstructionContext::Commit(roots, out)`으로 한다(A1 SCC 로드 — 여러 Stage를 한 트랜잭션에 올려 함께 공개하고, 같은 트랜잭션의 다른 그래프 객체는 `ConstructionContext::FindObject`로 찾는다). 호출 검사(순서 InvalidArgument·진행 중 재진입 Busy·소유 스레드·Registry·접근 문맥·트랜잭션) 실패는 상태를 바꾸지 않아 올바른 호출로 계속할 수 있고, 호출 검사 뒤 작업이 실패하면 이후 단계는 InvalidArgument다. 트랜잭션에 객체를 넣은 뒤의 실패(CreateObjects 도중의 Create 실패·ApplyFields·CheckRequired)는 트랜잭션을 그 오류로 고정한다(`ConstructionContext::Fail` — Commit이 실패하고 Rollback·소멸자가 되돌린다. 한 트랜잭션에 올린 Stage 하나의 실패가 트랜잭션 전체를 실패시킨다). 노드 표는 그래프의 NodeTable 또는 `ObjectGraphInput::nodes`(Exports에서 만든 `GraphNodeDesc`) 중 하나이고, 기준 Outer는 `hasBase`·`base` 요약(곧 만들 패키지면 `OuterSummary::ForNewOutermost(true)`)으로 기술한다. Stage는 고아 노드를 허용한다(호출자가 root·Outer로 붙잡는다). 생성자·CreateObjects·ApplyFields·CheckRequired는 owner thread 전용이고, `Validate`는 ObjectRegistry에 닿지 않아(Registry 정책은 생성자가 사본으로 둔다) worker에서 부를 수 있다 — 그 스레드에서 1단계 hook·마이그레이션 콜백이 실행되고 allocator가 쓰이므로 그에 맞는 스레드 안전성이 필요하다(RequiredMissing은 owner thread에서 온다). hooks·migrationUser는 마지막 단계(CheckRequired)까지 살아야 하고 allocator는 스레드 사이의 할당·해제를 지원해야 한다(worker 전용 arena 불가). Stage는 입력 byte·노드 이름을 빌린다.

`LoadObjectGraph`는 Stage 위의 편의 API로, NodeTable이 있는 그래프를 한 트랜잭션으로 복원하고 노드 1을 root로 Commit한다. BaseOuter 비트가 있는 그래프는 `options.baseOuter`(생존 객체 — 그 `DescribeOuter`가 Stage의 base 요약)가 있어야 하고 비트가 없으면 없어야 한다. externalCount는 0이어야 한다(외부 참조 대상을 줄 수 없다 — A0-Oe).

1. header·NodeTable·Record 경계와 남는 byte, 개수·작업 한도를 확인하고 노드 출처(NodeTable과 노드 설명 중 하나, BaseOuter 비트와 hasBase 일치)를 본다.
2. 노드마다 타입을 Acquire하고 typed layout과 outer 값(자기 번호 미만)을 확인한 뒤, 노드 표의 정체성 규칙(Registry 정책 사본으로 `detail::CheckNewObjectIdentity` — WorldOnly·패키지 규칙·이름 형식·빈 이름·대표 자식·경로 4 KiB, Outer 요약은 앞 노드들과 base로 계산)과 그래프 안 형제 유일성((outer, 비교 키) — 중복은 파일 손상이라 InvalidArgument)을 생성자 전에 판정한다. 전체 field 값을 소유 형태로 미리 decode한다.
3. version 불일치는 기본 Unsupported다(높은 버전은 `LoadNewerSkippingUnknown` 정책). 옵션 migration callback이 있으면 생성 전에 호출하고 반환 record를 현재 schema/version으로 다시 검증한다. callback은 전달 allocator로 소유 Array를 반환한다.
4. local reference 범위/대상 native type을 모든 노드에서 검사한다(여기까지가 Stage `Validate`). 편의 복원은 이어서 노드 1에서 영속 강한 참조·inner → outer(노드 표의 outer)·패키지 정책 Registry의 outer → inner와 기준 Outer 아래 보존으로 닿지 않는 노드가 있으면 거부한다(공개 뒤 다음 GC가 조용히 지우지 않게). 패키지 정책이고 기준 Outer가 없으면 노드 1이 아닌 outer 0 노드(다른 패키지 — root 없이 공개된다)도 거부한다. unknown field를 skip한 결과 노드가 닿지 않게 되는 경우도 같은 규칙을 적용한다.
5. (2a) 만들기 전에 Registry 상태를 확인한다: 기준 Outer의 실제 요약(isOutermost·outermostIsPackage·pathBytes)이 base와 같아야 하고(InvalidArgument), 기준 Outer 노드는 실제 Inner 상태로 대표 자식 규칙을 다시 판정하며, 기준 Outer 아래 이름(대표 노드는 빈 비교 키)과 기준 Outer가 없을 때 outer 0 이름 있는 노드의 최상위 이름 점유는 AlreadyExists다. 이어서 ConstructionContext에서 모든 노드를 노드 표의 Outer·이름으로 생성한다(이제 할당·슬롯 상한·종료·재진입으로만 실패한다). (2b) scalar와 local-reference fixup을 적용한다.
6. (2c) Required 실효 값을 진단한다(hooks가 있을 때 — Docs/Design/ObjectFoundations.ko.md 4절): Required를 품은 최상위 필드만 저장과 같은 encode 경로의 검사 모드로 staged 객체에서 읽어(출력은 버린다) null인 Required 위치마다 `RequiredMissing`을 한 번 보고한다. 이 검사 동안 트랜잭션에 검사 중 표시가 서서 그 트랜잭션의 쓰기·Commit은 Busy다. 검사 writer는 필드마다 비워 다시 쓰고(`ArchiveWriter::Clear` — 파일에 있는 Required를 품은 가장 큰 필드 크기로 `Reserve`해 둔다) 크기·작업량 한도가 없다. struct 값은 Required를 품지 않는 멤버를 쓰지 않는다.
7. `options.prepare`가 있으면 노드 handle로 부른다(편의 복원 전용 — Transient 필드·AssetId 설정, 실패하면 rollback. Stage 호출자는 Commit 전에 직접 한다).
8. 노드 1을 root로 한 번에 Commit한다.

어느 단계든 실패하면 root를 반환하지 않고 생성 역순으로 rollback한다. 이는 allocator 누적 통계나 slot generation을 이전 수치로 되돌린다는 뜻이 아니다. Record에 없는 known field·struct 멤버는 정책 없이 생성자 값(기준)을 유지한다(누락 = 기준 — NotFound 없음). `policy.excludedFlags`(쿠킹 로드의 EditorOnly)의 필드·멤버가 있으면 생성자 전에 InvalidArgument다. Required는 로드에서 거부하지 않고 6단계의 실효 값으로만 진단한다(빠졌다고 null로 보지 않으므로 생성자가 non-null로 둔 Soft 참조가 빠져도 진단하지 않는다). 사용자가 전달한 TypeRegistry가 ObjectRegistry의 등록 저장소와 다르면 WrongContext다. 참조의 native type 제한은 기존 staged setter의 정확한 타입 검사를 따른다.

## 한도와 한계

기본 한도는 노드 4,096개, header 외부 참조 수 65,536, 처리한 TLV 총 1,048,576개(중첩 struct TLV 포함 — Record 하나의 필드 수 한도도 남은 누계), 배열 원소 총 4,194,304개(배열 하나는 1,048,576개 이하 — 로드는 필드별 MaxElements를 걸지 않는다), 파일 64MiB, 개별 encoded field 64MiB(배열 payload 전체 포함), work 8 × (필드 누계 + 원소 누계 + 파일 byte) ≈ 5.8억 단위, migration 출력 누계 64MiB다. 바이트·필드·원소 상한을 각각 채운 입력이 기본 work 안에서 저장·복원된다(저장은 byte당 약 5–7, 복원은 약 6 단위를 청구한다 — 시험 GraphLimitTests). 원본과 migration 결과를 모두 파싱하면 양쪽 field 수를 처리 누계에 포함한다. lookup, index 초기화, codec bytes 복사·검사와 fixup 비용을 bounded budget에 포함한다. migration callback의 자체 CPU/전역 할당·native 생성자와 accessor 내부의 임의 코드는 신뢰 계약이다.

maxBytes는 전체 RAM peak가 아니다. 저장은 필드 writer·record staging·최종 output 중 둘이 단계마다 함께 있어 약 2배(약 128MiB — Record·필드 writer 한도가 남은 파일 byte라 노드 Record의 합이 maxBytes로 묶인다, writer 성장 중에는 잠깐 더)이고, 복원은 input·index·decoded 문자열·migration output·staged 객체와 Required 검사 버퍼(Required를 품은 가장 큰 필드 하나의 검사 출력 — 성장 중에는 옛·새 버퍼가 함께 있어 그 약 2.5배)를 동시에 보유할 수 있다. 값 임시 저장의 단일 할당 한도도 maxValueBytes(64MiB)다. 각 제한과 객체 수가 이를 제한하며 allocator 자체의 메모리 quota는 별도 정책이다.

소프트 참조의 해석·로드(대상 에셋 찾기), 외부 참조(A0-Oe), 임의 컨테이너/숨은 native field, 외부 IO, 압축·암호화, 비동기 저장, 실행 중 graph mutation snapshot은 후속 범위다. 이 기능들을 암묵적으로 raw memory 저장이나 부분 성공으로 대체하지 않는다.

## 참조 타입 사전 검증

강한 참조(ObjectReference)에만 해당한다. 소프트 참조의 `value.type`(기대 타입)은 저장·복원에서 검사하지 않는다(대상을 찾을 때 검사한다). nonnull 참조의 `value.type`이 0이 아니면 해당 등록 lease의 native type과 실제 대상의 native type을 비교한다. 파생 타입의 `IsA`만으로 참조를 허용하지 않으며 모든 생성자 실행 전에 TypeMismatch로 거부한다. 동일 native type을 다른 TypeId로 등록한 별명은 허용하여 기존 Registry setter 규약과 일치시킨다. `value.type=0`은 기반 Object의 일반 참조다. nonnull 참조의 명시 대상 타입이 등록되지 않았으면 NotFound를 반환한다. null 참조는 대상 등록을 요구하지 않는다.
