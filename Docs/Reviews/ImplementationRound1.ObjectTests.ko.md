# Implementation Round 1 — Object 독립 검증 결과

상태: R1 객체 수명 subset의 독립 테스트 작성 및 구현 교차 검토. 설계 기준은 CoreObjectInputDetailedPlan의 §9–14 및 R1 CT10/13–17/20–22이다. 실제 실행 증거와 미실행 범위는 마지막 절에 구분한다.

## 범위

단일 owner lane, nonmoving 객체 저장소, 수동 타입 등록, Root/Weak/Borrow, 비공개 생성과 rollback, full STW graph 수거, 논리 파괴와 보존 수명의 분리, Busy shutdown을 검증한다. 병렬/점진 marker·VM·renderer·실제 GPU/오디오·플랫폼 SDK를 구현하지 않는다. backend 완료는 독립 fake hold로 표현한다. Windows의 실제 실행 결과와 macOS 미실행을 구분한다.

## 독립 fixture와 필수 반례

| 파일 | 필수 확인 | 독립 판정 근거 |
|---|---|---|
| MetadataTests.cpp | source field/name/attribute 변경 뒤 deep-copy 보존, duplicate type·foreign token 거부, N-fail rollback, reader 존재 시 unregister Busy, 마지막 consumer 종료 뒤 code lease 해제 | 등록 전 원본 값의 고정 사본·allocation baseline·code retain/release 횟수 |
| RegistryTests.cpp | null·stale generation·다른 registry domain·잘못된 context·잘못된 type 거부, chunk 경계, nonmoving 주소, owner/target 검증 | 관찰한 handle/address와 독립 expected identity 목록·복사 우회 compile-time 거부 |
| ConstructionTests.cpp | 미공개 객체 Resolve 거부, Commit 전 scope 종료·할당 실패 rollback, 역순 1회 소멸, 실패 후 slot/root/type lease 회수, context 유지 중 GC Busy | ctor/dtor event 배열·공개 객체 수·allocator baseline |
| GcTests.cpp | root가 있는 cycle/shared-child/deep graph 보존, root 제거 후 회수, unrooted cycle 회수, GC scratch 할당 실패 시 살아 있는 객체 무손상 | 별도 adjacency 배열의 반복 DFS, registry visitor 구현 재사용 금지 |
| LifetimeTests.cpp | Root Clone/Reset, weak 승격, Borrow가 있는 Collect Busy, destroy 직후 Resolve/pin 거부, root/edge/backend hold 저장소 보존, 중복 destroy/hold Reset 무해, Shutdown Busy 후 회복 | destructor counter·weak/Resolve 결과·live/root/hold count·정상 종료 후 allocation baseline |
| ThreadOwnershipTests.cpp | foreign thread의 Pin/Resolve/Borrow Get/Collect 거부, Root/Backend Reset 허용 | 실제 OS thread join 뒤 결과 및 root/hold count |

NFailAllocator는 production allocator를 감싸고 특정 allocation 시도만 실패시킨다. Deallocate는 실패 주입하지 않는다. fixture는 실패 지점 직전 counter를 reset하고 각 실패 반복을 별도의 registry/allocator 상태에서 시작한다. successful run의 실제 allocation 수를 먼저 확인해 모든 할당 지점을 순회하되, API가 정한 실패 이전 상태 보존·정확한 정리를 기준으로 판정한다.

## 리뷰 차단 기준

- 객체의 논리 유효성과 storage 보존을 혼동하지 않는다. destroy 후 existing RootRef 보존은 허용되지만 새 pin과 일반 Resolve 성공은 실패다.
- generation만 검사하여 다른 registry/context 객체를 잘못 반환하면 실패다. scope 값이 없는 요청으로 scoped 객체를 우회할 수 없어야 한다.
- Borrow가 살아 있는 동안 수거/파괴가 진행되어 pointer가 무효화되면 실패다. R1에서 Busy 거부는 정당한 baseline이다.
- 생성 실패·metadata 등록 실패가 TypeId/slot/root를 반쯤 공개하거나 이전 등록을 변경하면 실패다.
- full collector는 반복 worklist를 사용한다. 독립 DFS가 도달한 객체를 수거하면 즉시 실패다. 보수적 잔존을 성공으로 숨기지 않고 다음 정리 단계의 eventual reclaim을 별도 확인한다.
- GC scratch 부족은 불완전 mark 결과로 sweep해서는 안 된다. Busy/OutOfMemory 같은 명시 실패와 기존 객체 보존을 확인한다.
- metadata code/data lease, backend hold, RootRecord의 해제 순서를 destructor counter와 Busy 결과로 확인한다. timeout은 완료 증거가 아니다.
- 테스트가 implementation의 동일한 graph traversal을 oracle로 다시 호출하지 않는다. 부정 입력을 실제로 실행하고 결과를 확인한다.

## 실행 기록 규칙

각 테스트는 TEST/CHECK/REQUIRE 하네스를 사용한다. REQUIRE는 현재 void test에서 조기 return하므로 fixture와 로컬 RAII 변수는 실패 경로에서도 소유 자원을 정리하도록 설계한다. 파일 내부 helper는 REQUIRE의 조기 return 의미를 오용하지 않고 필요한 상태를 Result/boolean으로 반환한다. 실패한 결과의 Value를 읽지 않는다.

실행 후 이 보고서에는 빌드/실행 command의 의미, seed/규모, 통과·실패·미실행, 발견한 구현 오류와 수정 closure를 추가한다. Windows 실행은 root가 수행하고 그 실제 로그를 근거로 기록한다. macOS 미실행·sanitizer 미실행을 PASS로 합산하지 않는다.

## 구현 교차 검토와 수정

| ID | 발견한 문제 | 수정·회귀 증거 |
|---|---|---|
| IR1-O01 · P1 | ObjectRef의 implicit public copy/move가 owner-aware SetReference를 우회하고 Object identity도 일반 복사할 수 있었다. | Object/ObjectRef의 copy/move constructor/assignment 삭제. RegistryTests의 compile-time negative fixture는 참조 필드와 참조 필드가 없는 Object 파생형을 각각 검사한다. |
| IR1-O02 · P1 | 기존 RootRef의 Clone이 신규 pin과 같은 CheckLive를 사용하여 DestroyRequested 저장소의 보존 이전을 거부했다. | RetainRoot는 기존 root>0과 Alive/DestroyRequested를 확인하고 신규 TryPin은 Alive만 허용한다. ObjectDestroyEndsAccessButRetainsRootedStorage에서 논리 접근 거부와 기존 lease Clone 보존을 동시에 검사한다. |

ObjectRegistry.cpp, ConstructionContext.cpp, TypeRegistry.cpp와 공개 수명 header를 읽어 다음 연결을 검토했다. GC work capacity 확보 실패는 mark/sweep 전에 반환하고, visitor는 registry gate 밖에서 호출한다. mark closure 전 오류 시 sweep을 실행하지 않는다. Commit은 모든 staged handle/edge를 검증한 뒤 같은 gate에서 publish하고 실패 context는 rollback ownership을 유지한다. retired 객체의 backend hold는 GC root가 아니며 storage 반환만 지연한다. destructor·allocator callback은 registry gate 밖에서 실행하며 nested collection은 Busy다. type revision의 code/data는 마지막 consumer가 끝날 때까지 보존된다.

GC oracle는 production visitor/collector를 재사용하지 않는 별도 adjacency 배열의 반복 DFS다. 128개 graph의 각 32개 component는 first/second 두 edge 경로를 모두 방문해야 전체가 도달 가능하도록 구성했다. 둘 중 한 visitor 경로를 생략하면 live set이 oracle와 달라진다. 별도의 2,048개 chain은 C++ recursive 방문 없이 전체 보존·회수가 이루어지는지 검사한다. 이것을 모든 graph 규모의 성능 보장으로 해석하지 않는다.

## 현재 R1 구현의 명시적 제한

- GC는 single owner lane의 full STW 기준 구현이다. parallel/incremental marking·VM root map·언어·실제 renderer/backend는 구현하지 않았다.
- ConstructionContext가 활성인 동안 Collect는 Busy다. 긴 미공개 graph 단계 사이에 GC를 허용하는 완전한 private-root provider는 후속이다. caller의 수동 초기화 오류는 Commit 전에 rollback한다. noexcept constructor 내부의 업무 실패를 예외로 시뮬레이션하지 않는다.
- native type 검사는 exact type 및 Object base 접근이다. 전체 reflected IsA hierarchy·ClassDefaults/instancing·Archive/Clang 생성기·prefab은 미구현이다.
- RootRef는 move-only이고 기존 lease의 복제는 명시 Clone의 Result로 수행한다. 현재 root 보존은 slot count 기반이며 계획의 완전한 RootRecord/provider 체계와 동일 완료로 표시하지 않는다.
- 청크는 fixture 친화적인 64 slots이며 linked lookup을 사용한다. 64K-slot production tuning·큰 registry 성능 최적화·p99 GC 목표를 검증하지 않았다.
- BeginDestroy/EndPlay 전문 hook은 미지원이다. RequestDestroy는 논리 종료, 최종 noexcept destructor와 backend hold 회수가 현재 수명 경계다.
- 미종료 root/borrow/backend hold가 있으면 Shutdown은 Busy이고 admission을 닫는다. registry 자체는 모든 lease보다 오래 살아야 하며 잘못된 강제 scope 종료를 안전한 회수로 위장하지 않는다.

## 실행 증거

2026-11-10 Windows x64 clang-cl 21.1.6의 실제 로그를 독립 확인했다. 첫 Debug 실행은 seed 7351, repeat 1, 전체 51개 test에서 failed_tests=0/failures=0였다. 마지막 code lease fixture를 추가한 뒤의 `out/windows-clang-asan/foundation-report.json`은 전체 52개 test, failed_tests=0/failures=0이며 `ObjectMetadataRetainsCodeUntilLastReaderAndUnregisterCompletes`의 PASS를 직접 확인했다. Object test 27개 중 본 담당 25개와 별도 담당 Diagnostics 2개를 구분한다.

ASan의 `Testing/Temporary/LastTest.log`에서 foundation, harness_negative_controls, runtime_stl_audit, scaffold_transaction 4 CTest suite 통과를 확인했다. `build-evidence.json`은 address sanitizer, sourceStableDuringRun=true, source digest `eb88c61664981ba753a37333d6baa2c5c6e39844822e08aa8b31fef200f3d014` 및 현재 runner report 일치를 기록한다. 실제 실행은 build 담당이 수행했고 이 담당은 원문 로그와 증거 파일을 읽어 검증했다. 최종 Debug/Release 재실행 증거는 별도 build 검토 보고서가 관리한다.

macOS·UBSan/TSan·병렬 GC·실제 GPU는 이 담당의 실행 확인 범위 밖이며 미실행이다. ASan 통과를 데이터 경쟁 검증이나 임의 크기 graph의 성능 보장으로 해석하지 않는다. 이 결과는 전체 Foundation Gate 또는 문서의 UE5급 범위 완료를 뜻하지 않는다.

## 한국어 계약 주석 교차 검증 — 2026-11-10

ObjectRuntime 공개 API와 구현, MetaSchema 및 `Docs/CodingStyle.md`의 새 주석 기준을 독립 대조했다. 공개 계약을 헤더 가까이에 두고 소유권·비소유 뷰 유효 기간·스레드 제한·오류 시 상태를 설명한다는 기준을 확인했다. 이번 검토는 주석의 정확성 검증이며 실행 검증을 대체하지 않는다.

| 검토 항목 | 수정 내용과 구현 대조 | 판정 |
|---|---|---|
| Shutdown 실패와 닫힘 상태 | `ObjectRegistry.h`는 접근 검증·재진입 거부가 admission을 닫기 전에 반환함을 구분했다. `ObjectRegistry::Shutdown`의 CheckAccess/processing 검사 → closed 설정 → Collect 순서와 일치한다. | 해결 |
| 콜백 재진입의 허용 범위 | `RuntimeTypeOps.h`는 생성·간선 변경·수집의 Busy 거부, lease 반환·통계 조회 허용, 활성 ConstructionContext 롤백/소멸 재진입 금지를 구분했다. Root/Borrow/Backend 계수 반환과 Rollback의 치명 오류 경로를 함께 대조했다. | 해결 |
| TypeId 용어 | `TypeSchema.h`의 id를 안정적인 타입 식별자로 바로잡아 자산 식별자와 구분했다. 스키마 version과 native type key를 구분하는 나머지 계약도 유지한다. | 해결 |
| Borrow/BackendLease 타 스레드 반환 | `ObjectRefs.h`는 안전하게 소유권을 이관한 뒤 반환하고 같은 래퍼의 Get/이동/Reset을 동시에 사용하지 않도록 명시했다. 계수의 잠금이 래퍼와 사용자 객체 전체를 동기화한다는 보장으로 확대되지 않는다. | 해결 |

RootRef의 기존 보존권 복제와 신규 pin의 구분, Borrow의 raw pointer 유효 범위, ObjectRef의 owner-aware 변경, ConstructionContext의 미공개 생성·롤백, Collect의 부분 표식 실패 시 sweep 금지, TypeLease의 code/data 수명 주석에서도 추가 불일치를 확인하지 못했다. 이번 수정 확인은 읽기 전용으로 수행했으며 런타임이나 테스트 코드는 변경하지 않았다. 이후 String 구현에 대한 독립 검토와 최종 빌드 증거는 별도 범위다.
