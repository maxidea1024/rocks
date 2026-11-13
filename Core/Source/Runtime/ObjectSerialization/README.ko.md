# Rocks 단일 루트 객체 그래프 저장과 복원

`rocks_object_serialization`은 ObjectRuntime과 Serialization을 연결한다. 공개 API는 `SaveObjectGraph`와 `LoadObjectGraph`다. 첫 구현은 owner thread에서 완전한 typed metadata로 표현되는 단일 strong graph를 처리한다. source 코드와 시험 fixture는 구현 범위를 설명하며 실제 플랫폼별 통과 여부는 별도 통합 검증 보고서를 따른다.

## 지원 범위

bool, uint8/16/32/64, int32/64, float32/64, Guid, UTF-8 String, 강한 ObjectRef, AssetId(Guid wire), Sha256Digest(Bytes wire, 정확히 32 byte), SoftObjectRef(SoftObjectReference wire), 리플렉션 enum(I64 wire)을 지원한다. enum의 선언되지 않은 값은 저장에서 InvalidArgument이고, 복원은 기본 정책에서 거부하며 `ReportAndDefault`면 진단 후 대체 값을 쓴다. 소프트 참조는 값으로 저장하며 대상을 그래프에 넣거나 찾지 않는다. 저장(C++에서 직접 대입한 값)과 복원 모두 null 규칙(asset 0에 경로가 있으면 안 됨)과 4 KiB 경로 상한을 검사해 InvalidArgument로 거부한다. 좁은 unsigned는 U32, signed는 I64 wire 형식을 사용하고 복원 시 실제 값 폭을 검사한다. unknown GUID field는 기본 진단 후 skip한다(`UnknownFieldPolicy::Reject`면 거부). known field의 wire tag·값·trailing byte는 엄격히 검사한다.

`RuntimeTypeOps::completeFieldLayout`이 true이고 schema와 필드 codec(`RuntimeFieldOps::encode`·`decode`)이 stable field GUID별 1:1이어야 한다. 값은 등록된 codec이 쓰고 읽으며, 복원은 두 단계(1단계 검증·사전 청구, 2단계 생성·적용)이고 decode 정책·진단 hook·소프트 참조 수집은 `ObjectGraphSaveOptions`·`ObjectGraphLoadOptions`로 정한다(Docs/Implementation/Serialization.ko.md "필드 codec과 문맥"). 이 선언에는 상속 필드를 포함한 모든 영속 필드와 모든 강한 간선이 포함된다(ROCKS_NATIVE 멤버는 영속 필드가 아니고 강한 참조를 담을 수 없어 포함 대상이 아니다). 기본 수동 metadata는 false이며 부분 metadata·Opaque·누락 codec은 Unsupported다(Transient 필드는 codec이 없어도 된다). 수동 작성자가 거짓 완전성을 선언하는 임의 플러그인의 메모리 안전성을 검증하는 기능은 아니다.

객체 포인터나 ObjectHandle의 registry/slot/generation bytes는 저장하지 않는다. 각 객체에 1부터 시작하는 파일 내부 ID를 부여하고 참조는 u64 ID, null은 0으로 쓴다. 동일 객체를 중복 생성하지 않으므로 cycle과 shared identity가 유지된다. metadata 필드 순서 BFS에 따라 ID와 출력 순서가 정해진다. 원본과 다른 Registry로 복원해도 동등한 그래프를 다시 저장하면 같은 bytes가 된다.

## 포맷

외곽은 `ROCKSGRF` 8 bytes, format version u32=1, object count u32, root local ID u32=1이다. 이후 local ID u64(1..count 순서), TypeId Guid, schema version u32, `WriteBytes`로 감싼 `ROCKSREC` record를 객체 수만큼 쓴다. 내부 record의 schema/type/version은 외곽 선언과 일치해야 한다. unknown 외곽 format version은 Unsupported다.

## 저장 수명

Save 전체 동안 root Borrow를 유지하여 GC의 진행을 막고, 각 TypeLease로 getter 코드/metadata 수명을 보존한다. caller와 allocator/accessor callback은 source graph를 변경하지 않아야 한다. 마지막 output 할당 callback 뒤 root Get을 재검증하고 결과를 게시한다. 반환 Array는 caller allocator를 빌리며 source Registry를 참조하지 않는다.

String getter 복제에는 임시 bounded forwarding allocator를 사용해 큰 할당을 값 검사 전에 거부한다. 해당 FieldValue를 adapter보다 먼저 파괴하고 output에는 원 caller allocator만 보관한다. metadata/getter/allocator의 임의 외부 코드 자체를 강제 중단하는 sandbox는 아니다.

## 복원과 실패 보장

1. 외곽 전체 구조와 길이, 연속 local ID, 개수·작업 한도를 확인한다.
2. 모든 타입을 Acquire하고 typed layout을 확인한다. 전체 field 값을 소유 형태로 미리 decode한다.
3. version 불일치는 기본 Unsupported다. 옵션 migration callback이 있으면 생성 전에 호출하고 반환 record를 현재 schema/version으로 다시 검증한다. callback은 전달 allocator로 소유 Array를 반환한다.
4. local reference 범위/대상 native type와 root로부터의 도달성을 검사한다. 파일에 고아 객체가 있으면 거부한다. unknown field를 skip한 결과 객체가 고아가 되는 경우도 같은 규칙을 적용한다.
5. ConstructionContext에서 모든 객체를 생성한 뒤 scalar와 local-reference fixup을 적용하고 첫 root를 한 번에 Commit한다.

어느 단계든 실패하면 root를 반환하지 않고 생성 역순으로 rollback한다. 이는 allocator 누적 통계나 slot generation을 이전 수치로 되돌린다는 뜻이 아니다. `allowMissingFields=false`가 기본이며 누락 known field는 NotFound다. true이면 해당 필드의 native 기본값을 유지한다. 사용자가 전달한 TypeRegistry가 ObjectRegistry의 등록 저장소와 다르면 WrongContext다. 참조의 native type 제한은 기존 staged setter의 정확한 타입 검사를 따른다.

## 한도와 한계

기본 한도는 객체 1,024개, 처리한 TLV 총 4,096개, 파일 16MiB, 개별 encoded field 4MiB, work 64Mi 단위, migration 출력 누계 16MiB다. 원본과 migration 결과를 모두 파싱하면 양쪽 field 수를 처리 누계에 포함한다. lookup, index 초기화, codec bytes 복사·검사와 fixup 비용을 bounded budget에 포함한다. migration callback의 자체 CPU/전역 할당·native 생성자와 accessor 내부의 임의 코드는 신뢰 계약이다.

maxBytes는 전체 RAM peak가 아니다. 저장은 record staging과 최종 output을, 복원은 input·index·decoded 문자열·migration output·staged 객체를 동시에 보유할 수 있다. 각 제한과 객체 수가 이를 제한하며 allocator 자체의 메모리 quota는 별도 정책이다.

소프트 참조의 해석·로드(대상 에셋 찾기), 다중 root, 임의 컨테이너/숨은 native field, 외부 IO, 압축·암호화, 비동기 저장, 실행 중 graph mutation snapshot은 후속 범위다. 이 기능들을 암묵적으로 raw memory 저장이나 부분 성공으로 대체하지 않는다.

## 참조 타입 사전 검증

강한 참조(ObjectReference)에만 해당한다. 소프트 참조의 `value.type`(기대 타입)은 저장·복원에서 검사하지 않는다(대상을 찾을 때 검사한다). nonnull 참조의 `value.type`이 0이 아니면 해당 등록 lease의 native type과 실제 대상의 native type을 비교한다. 파생 타입의 `IsA`만으로 참조를 허용하지 않으며 모든 생성자 실행 전에 TypeMismatch로 거부한다. 동일 native type을 다른 TypeId로 등록한 별명은 허용하여 기존 Registry setter 규약과 일치시킨다. `value.type=0`은 기반 Object의 일반 참조다. nonnull 참조의 명시 대상 타입이 등록되지 않았으면 NotFound를 반환한다. null 참조는 대상 등록을 요구하지 않는다.
