# ObjectGraph codec 독립 검토

검토일: 2026-11-10. 대상은 `ObjectGraph.h`, `GraphInternal.h`, `GraphValues.cpp`, `GraphSave.cpp`, `GraphLoad.cpp` 및 연결된 typed accessor·TypeRegistry·ConstructionContext 계약이다. codec 구현 작성자와 다른 검토자가 원문을 읽고 반례와 독립 통합 시험을 작성하였다. 이 기록 시점의 시험 파일은 저장·서식 검사까지 완료하였으며, 통합 컴파일과 실행 판정은 빌드 담당자의 후속 결과로 확정한다.

## 확인한 결함과 수정 판정

**OG01 / P2 — 참조 사전 검증과 native setter의 불일치. 수정 후 소스 검토 PASS, 실행 검증 대기.**

수정 전 `GraphLoad.cpp`의 `ValidateReachability`는 `referencedType`에 대해 `TypeLease::IsA`만 검사하였다. 반면 `ObjectRegistry::SetEdge`는 정확한 native type을 요구한다. 따라서 `ObjectRef<Base>` 필드에 reflected `Derived`의 local ID를 넣으면 사전 검증을 통과하고 생성자들이 실행된 뒤 staged setter에서 `TypeMismatch`가 발생하였다. 트랜잭션의 공개 원자성은 유지됐지만, 공개 계약의 ‘전체 구조·값·참조 검증 뒤 생성’은 만족하지 않았다.

별도로 `TypeRegistry::Register`는 TypeId 중복만 거부하므로, 동일 native type을 다른 TypeId로 등록할 수 있다. IsA 기준을 그대로 강화하면 실제 setter가 허용하는 이런 별명 참조를 저장 단계에서 계속 거부하게 된다.

수정 후 저장과 복원 양쪽에서 명시된 referencedType의 등록 lease를 얻고, 그 native type을 실제 대상 lease의 native type과 비교한다. Save는 `ObjectRegistry::AcquireRegisteredType`을 사용한다. Load는 `ValidateReachability` 안에서 검사하므로 생성 이전에 실패한다. null 참조는 대상 등록을 요구하지 않으며 referencedType 0의 기반 Object 참조는 generic으로 유지한다. 명시된 nonnull 대상 등록이 없으면 `NotFound`를 반환한다. 비교에 쓰는 lease가 비교 범위를 지키며 실제 객체 타입 lease는 복원 전체 단계 동안 보유된다.

수용 기준은 다음과 같다.

- Base 필드에 Derived local ID를 입력하면 `TypeMismatch`, 생성자 호출 수 0, 생성 chunk 수 0, live/reserved 객체 수 0, 임시 할당과 lease 증가 0이다.
- 같은 native Base를 다른 TypeId로 등록한 대상은 저장·복원할 수 있고, 재저장·재복원 후에도 그 별명 TypeId와 필드 값이 유지된다.
- generic Object 및 null 참조의 기존 허용 범위가 좁아지지 않는다.

## 수명·원자성 검토 결과

- Save는 루트 Borrow를 끝까지 유지하여 GC slice를 막고, owner thread와 변경 금지 callback 계약 아래에서 BFS 순서를 결정한다. BFS 중 배열이 재할당되어도 이전 SaveNode 주소를 보관하지 않고 index로 다시 접근한다. 필드 임시 allocator보다 FieldValue가 먼저 파괴되며 반환 출력은 이 adapter를 참조하지 않는다.
- LoadNode는 원본의 비소유 record span과 migration 결과의 소유 Array를 구분한다. migration 결과는 현행 schema/version/TLV로 다시 검사하며, decoded 문자열은 복사한다. 실제 객체 String의 staged setter는 객체의 allocator를 사용하므로 decode scratch allocator가 객체 안으로 넘어가지 않는다.
- 모든 node의 구조, wire type, scalar 폭, UTF-8, local ID 범위와 루트 도달성을 검사한 다음 ConstructionContext를 시작한다. unknown field는 값 해석 없이 넘기되 record envelope 경계 검증을 거친다. 고아 객체 및 범위 밖 참조는 생성 전에 거부한다.
- 생성 후 실패 가능한 할당, typed setter, fixup, commit 검증이 남아 있다. 이 실패들은 비공개 상태를 유지하고 역순 rollback한다. 사전 검증 성공이 생성자나 할당의 성공을 보장한다는 뜻으로 해석하지 않았다.
- 생성 트랜잭션은 원본 input, decoded value, migration buffer가 살아 있는 동안 실행되고, 최종 객체는 그 저장소를 빌리지 않는다. TypeRegistry 및 registry allocator가 결과 객체보다 오래 사는 기존 계약은 계속 필요하다.
- object/field/byte/migration/work 한도 및 합산 overflow 검사를 확인하였다. work는 codec 반복·복사 비용이며, 신뢰된 생성자·접근자·migration callback 자체의 임의 실행 시간을 제한하는 보장이 아니다. 이 제한이 공개 주석에 명시되어 있다.

위 범위에서 OG01 외에 확정된 메모리 수명 또는 부분 공개 결함은 발견하지 않았다. 이는 임의 플러그인 callback의 안전성이나 미지원 외부·weak 참조, 비동기 IO까지 검증하였다는 뜻이 아니다.

## 추가한 독립 통합 시험

`Tests/ObjectSerialization/ObjectGraphReviewTests.cpp`에 다음 3개 시험을 추가하였다. codec 작성자 시험의 수동 RuntimeFieldOps 경로와 구분하여 실제 `ReflectionFixture.generated.h` 출력을 사용한다.

1. `ObjectGraphGeneratedInheritanceRoundTripOutlivesSourceAndScratchAllocator`: 상속된 GUID/int32, 지원 scalar의 경계 값, UTF-8 및 내부 NUL String, 2-node cycle을 저장·복원한다. source registry와 입출력 buffer, decode scratch allocator가 파괴된 뒤에도 target의 값과 간선이 유지되는지 검사한다. 다른 registry에서 재저장한 byte 순서가 같고, generated Trace를 통한 GC 보존·최종 회수가 성립하는지 확인한다.
2. `ObjectGraphNativeReferenceMismatchIsRejectedBeforeAnyConstructor`: 수동 BaseHolder의 typed Base 참조에 실제 generated Node 파생 타입을 연결한 파일을 조립한다. 실패 후 살아 있는 객체 수만 검사하지 않고 생성자 호출 수, chunk 수, 임시 할당과 lease 원상 복구를 검사한다.
3. `ObjectGraphReferenceToNativeAliasTypePreservesAliasSchemaOnResave`: 같은 native Base의 다른 TypeId 등록을 사용한다. load, native Resolve, 값 변경, save, 두 번째 registry load를 거쳐 별명 schema ID와 값이 유지되는지 검사한다.

이 파일의 clang-format 검사에는 통과하였다. 본 검토자는 빌드를 병렬 실행하지 않았으며, 위 3개 시험의 실제 PASS는 후속 통합 결과를 기록한 뒤에만 주장한다.

## 후속 통합 실행 확인

위 기록 이후 최종 소스 `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`로 Windows Debug·Release·ASan 전체 179개와 CTest 10/10/11개가 통과하였다. 이 보고서의 새 독립 회귀도 실제 실행 목록과 PASS에 포함된다. 같은 소스의 Release 100회 반복 17,900회는 실패 0이다. 원기록의 실행 대기는 검토 당시 상태이며, 최종 로컬 실행 근거는 [5차 검증 기록](ImplementationRound5.FoundationValidation.ko.md)을 따른다. macOS 결과는 별도 native CI 증거로 확정한다.
