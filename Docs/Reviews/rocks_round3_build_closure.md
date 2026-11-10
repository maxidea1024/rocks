# Rocks 구현 계획 Round 3 — 빌드·선행 조건 수정 재검증

검토일: 2026-11-10. root의 Round 2 일괄 수정 완료 통보 후 `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `Docs/Design/CoreObjectInputData.json`을 새로 읽었다. 원본은 수정하지 않았다.

## 결과

**R2B01과 R2B02는 문서 계약 및 작업 데이터에서 해결되었다.** 추가된 metadata snapshot과 InputReadKey 계약에서도 R1을 후속 String/Name·Input·World·VM·dynamic loader에 의존시키는 새 선행 조건은 확인하지 못하였다.

아래 PASS는 계획의 반례 검토 결과다. 엔진 컴파일, 실제 캐시 무효화, 메모리 안전성, 입력 장치 시험은 실행하지 않았다. 실제로 실행한 검사는 JSON의 작업 의존성·파일 소유 관계 확인이며 구현 검증을 대신하지 않는다.

## 반례별 재검증

| ID | 주입한 반례 | 최신 근거 | 판정과 남은 구현 검증 |
|---|---|---|---|
| R3B01 | C00부터 C12까지만 도입한 깨끗한 저장소에서 Registry의 mutex 공개 헤더가 C16을 요구한다. | 상세 15.1 작업표 675행은 C02 산출물에 `Public/Platform/Sync.h`를 명시한다. 689행은 C16을 “C02 Sync.h 확장”으로 표기한다. JSON `tasks[C02]`와 `files`에서 해당 파일의 최초 소유자는 C02 하나이며 C16 파일 목록에서는 제거되었다. 상세 5.1·15.1은 C02의 atomic/mutex와 C16의 thread/wait를 구분한다. | **PASS.** R0/R1에 필요한 공개 계약의 도입 시점과 문서 순서가 일치한다. 구현 때는 C00–C12만 포함한 Win/Mac fixture를 빌드하고 Sync 공개 계약이 EngineBase만 사용하는지 확인해야 한다. |
| R3B02 | source/schema/ABI가 같은 warm cache에서 `.clang-format` 또는 실제 읽은 설정만 변경하여 이전 formatted 출력이 hit로 복원된다. | 마스터 27.2의 1954행, 상세 9.3의 444행에 실제 style 설정 digest가 명시 입력으로 추가되었다. 단일 action 또는 raw→formatted 두 action 중 하나를 사용하고 최종 producer는 하나로 제한한다. CT12(상세 744행)는 config 변경과 stale hit 0을 포함한다. | **PASS.** config-only 변경을 별도 입력으로 인식하는 계약과 검증 책임이 연결되었다. 구현 시 clean/warm·local/remote 바이트 대조, 기존 artifact의 stale hit 거부를 수행해야 한다. |
| R3B03 | 설정은 그대로 두고 formatter 실행 파일만 바꾸거나 서로 다른 worker가 다른 formatter를 사용한다. | 같은 문장에 executable/version digest가 있으며 CT12는 binary-only 변경, 동일 최종 출력, 원격 stale artifact 거부를 구별한다. JSON C20의 장애 주입에도 `formatter/config 변경`이 추가되었다. 동일 최종 바이트에는 timestamp를 보존한다. | **PASS.** 도구 변경을 codegen 결과와 연결하지 않았던 누락이 해소되었다. 실제 빌드에서는 최종 파일의 단일 producer와 byte hash를 유지하고, formatter 실패를 성공 artifact로 게시하지 않는 기존 27.2의 실패 규약을 함께 검증해야 한다. 이번 검토는 캐시 정확성 계약 판정이며 실제 재컴파일 비용 측정은 아니다. |
| R3B04 | R1의 수동 TypeSchema를 scratch arena에서 등록하고 즉시 arena를 reset한다. 깊은 복사에 C13 String/Name이나 dynamic loader가 필요하여 R1 작업이 역행한다. | 상세 9.1의 432행과 마스터 7절의 538행은 registration-owned 깊은 복사, R1 C07의 Guid/POD 배열·Core 소유 byte buffer subset, C13을 기다리지 않는 순서를 명시한다. C05가 Array/Span을, C06이 Function·loader 비의존 ModuleLease를 먼저 제공한다. CT10(상세 742행)은 source arena reset/poison·old reader 중 unregister와 revision lease를 포함한다. | **PASS.** C07이 C13/C27에 역의존할 이유를 문서에서 제거하였다. metadata 저장소의 lease와 code ModuleLease를 구별하며 Busy unregister를 기본으로 유지한다. 구현에서는 poison 후 type/name/field 조회, 복사 실패 rollback, 소비자 종료 전 ops/data 보존을 시험해야 한다. |
| R3B05 | 같은 user/tick을 가진 UI와 PIE A/B 조회가 충돌한다. 이를 수정하려고 InputCore가 World/Widget을 직접 참조하거나 Core R1이 Input 초기화를 요구한다. | 상세 17.2의 825–834행은 InputReadKey 기반 조회와 caller-owned event 복사본을 사용한다. 18.1–18.2의 872–893행 및 마스터 1640행은 opaque scope/generation·domain·clock epoch·tick을 동일 history key로 사용하고 World/Widget 타입 도입을 금지한다. IT03/IT19는 scope 충돌·restart·지연 query를 포함한다. JSON I00은 C26을 선행 조건으로 가지며 C00–C12의 선행 목록에는 Input 작업이 없다. | **PASS.** 새 scope API는 Foundation을 기다리는 상위 Input 계약에 위치하며 R1의 선행 조건을 늘리지 않는다. 실제 Studio/PIE와 OS 장치 시험은 기존 외부 Gate가 완료된 뒤 수행하고, Synthetic 성공으로 대체하지 않아야 한다. |

## 실제 수행한 계획 데이터 검사

JSON을 파싱하여 작업 ID와 `선행작업` 관계 및 예정 파일 경로를 검사하였다.

- 작업 52개, 예정 파일 297개.
- 존재하지 않는 선행 작업 ID 0개, 작업 의존성 cycle 0개, 중복 예정 파일 경로 0개.
- C07의 전이적 선행 집합은 C00–C06, C12의 전이적 선행 집합은 C00–C11이다. C13 이후의 시스템이나 Input 작업이 이 집합에 들어오지 않는다.
- I00의 직접 선행 조건은 C26이다. Input의 scope/history 보강이 R1의 시작을 지연하지 않는다.
- `Engine/PlatformBase/Public/Platform/Sync.h`의 예정 파일 소유 작업은 C02 하나다.

이 검사는 명시된 작업 DAG와 파일 목록의 일관성만 보장한다. C++ include 그래프, 타입 정의의 완전성, 실제 링커 의존성은 엔진 구현 시 별도로 확인해야 한다.

## 종료 판정

이번 담당 범위의 Round 2 확정 항목 2건은 **문서상 closure PASS**다. 새 metadata/Input API를 포함한 다섯 반례에서도 추가 수정이 필요한 확정적인 작업 순서 역행은 발견하지 못하였다. 첫 구현은 예정대로 CPU-only Core와 오브젝트 수직 경로부터 시작할 수 있으며, 실제 최초 빌드 및 수명·캐시·플랫폼 검증은 아직 미실행 상태다.
