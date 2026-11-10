# Rocks 최종 교차 검토 — 빌드·플랫폼·헤드리스·작업 추적

검토일: 2026-11-10. 대상: `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `Docs/Design/CoreObjectInputData.json`. 상세 작업은 52개, 예정 파일은 293개로 확인하였다. 아래 행 번호는 검토 당시 기준이며 병행 수정으로 이동할 수 있다. 본문·생성기·작업 데이터는 수정하지 않았다.

이번 검토는 문서의 계약·선행 조건·완료 범위 대조다. 실제 엔진 실행, compiler/linker 성능 측정, 메모리·경쟁 상태 시험 결과가 아니다. P0/P1 설계 결함은 이 담당 범위에서 추가 확인하지 않았다. **수정이 필요한 P2 추적·범위 불일치 6건**과 소규모 표기 보완을 확인하였다. Core/GC의 세부 수명 검증은 별도 검토와 구분한다.

| ID | 우선 | 대상 | 판정 |
|---|---|---|---|
| FB01 | P2 | master §1.2 ↔ detailed §16.1·C26 | 같은 CF ID가 다른 검증 의미로 재정의됨 |
| FB02 | P2 | master §1.2/2.2 ↔ detailed §4.1·작업 플랫폼 | Linux의 필수 여부와 Foundation 시점이 다름 |
| FB03 | P2 | master §2.5 ↔ §27.4/27.9 | local fallback의 대상 범위가 교차 target의 host 제약과 불일치 |
| FB04 | P2 | master §14.10/22.5 ↔ §30 M4 | M4의 LT/TT gate에 후속 사례가 함께 포함될 수 있음 |
| FB05 | P2 | master §7.1 ↔ §22.4 | 새 코드 선언과 기존 타입 참조의 ID 생성 의미가 구분되지 않음 |
| FB06 | P2 | detailed I13·IT19·작업 JSON | 실제 Studio/PIE 통합 시험의 상위 fixture 조건이 누락됨 |

## FB01 · CF ID의 의미 충돌

**근거:** master 52–63행은 CF01을 독립성, CF02를 STL/API 정책, CF10을 메타/Archive, CF12를 지원 OS 통합으로 정의한다. detailed 679–690행은 같은 ID를 CF01 모듈/STL, CF02 allocator, CF10 종료/재등록, CF12 관측/재현성으로 정의한다. detailed C26(664행)과 작업 JSON C26은 `CF01–12` 완료를 산출물로 표기한다.

**구체 반례:** 상세안의 CT24 viewer 시험을 통과하여 CF12=PASS로 기록하면 master에서는 OS 통합을 통과한 것처럼 읽힌다. 상세 CF10의 종료 시험은 master CF10의 migration corpus를 대체할 수 없다. 기능 자체가 누락되었다는 주장보다 동일 ID의 의미가 다른 추적 오류다.

**수정안:** master CF01–CF12를 정본으로 유지한다. detailed §16.1의 이름·의미를 master와 맞추고 관련 CT 테스트를 다대다로 연결한다. 관측 시험은 CF11/CF12에 연결하되 OS 검증과 동일 시험으로 합치지 않는다. 작업·보고서에는 정본 gate ID와 실제 case ID를 함께 기록한다.

**수용 기준:** 모든 CF ID를 두 문서에서 같은 의미로 사용하고 C26의 CT01–24 결과를 master CF01–12 전체에 연결해야 한다. viewer 성공만으로 platform gate를 PASS로 만들 수 없어야 한다. 단순 ID 집합 일치뿐 아니라 이름·필수 사례·platform의 의미를 비교한다.

## FB02 · Linux baseline과 첫 Foundation 시점

**근거:** master 63행 CF12는 “Windows·macOS 및 Linux” 검증을 Foundation에 요구한다. 88행은 Linux x64 서버·기반 테스트를 baseline으로 표기한다. detailed 159행은 “Linux는 선택 확장 target”으로 선언하며 C00/C02/C16/C18/C26의 필수 플랫폼과 파일은 Windows/macOS다.

**구체 반례:** Win/Mac만 구현한 C26을 상세안 기준으로 완료하여 Foundation을 통과시키면 master CF12의 Linux 검증이 남는다. 반대로 master를 그대로 적용하면 예정된 Linux primitive/file/thread 작업이 없는 상세 DAG의 마지막 단계가 미충족 요구를 처음 발견한다.

**수정안:** 합의된 수정 방향은 **첫 Foundation은 Windows/macOS 필수, Linux CPU 기반은 M3, Linux server는 M5의 명시 필수 후속 gate**다. master CF12·platform 표와 detailed §4.1/후속 연결표에 이 시점을 일치시키고 Linux를 제품 전체의 선택 기능으로 표현하지 않는다. Linux 전체 Studio는 추가하지 않는다.

**수용 기준:** R2 결과에 Linux를 미실행 상태로 남겨도 Win/Mac Foundation 완료와 혼동되지 않아야 한다. M3 Linux CPU build/test와 M5 Linux server의 별도 필수 gate·선행 adapter·실행 환경이 추적되어야 하며, M5 제품 완료 때 Linux를 단순 optional skip으로 처리할 수 없어야 한다.

## FB03 · 교차 target의 local fallback 범위

**근거:** master 157행의 macOS→D3D11 shader cook은 Windows SDK worker를 사용할 수 있다. 160행도 compiler host가 지원하지 않는 Metal 도구 조합은 macOS worker로 배치한다. 그러나 1852행 Local executor는 “외부 agent 없이 전체 빌드 가능”, 1853행은 adapter unavailable 시 local 전환, 1919행은 “cache/network가 없어도 정확한 local build”를 target 제한 없이 선언한다.

**구체 반례:** macOS에서 Windows/D3D11 target cook을 요청하였으나 FXC를 제공하는 Windows worker가 없다. 이 action을 임의 local fallback으로 전환해도 필요한 host 실행 파일·SDK가 없으므로 완료할 수 없다. 다른 toolchain 산출물을 재사용하면 target/cache 계약까지 달라진다. 두 OS의 native 개발 지원이 모든 교차 target의 무조건적 offline 지원을 뜻하지는 않는다.

**수정안:** local 완결성은 **현재 host에서 지원되는 native/local toolchain target**으로 한정한다. action에 target triple뿐 아니라 compiler host 요구·허용 executor 집합을 기록한다. cross-target 작업은 적합 worker가 없으면 capability 오류를 반환하고, 검증되지 않은 대체 compiler·foreign executable 실행으로 전환하지 않는다. Windows 전용 remote executor가 없어도 Mac native build는 독립 실행한다.

**수용 기준:** Win native·Mac native offline build는 성공해야 한다. Mac에서 Windows worker가 없는 D3D11 cook은 명시 unsupported/capability 실패이며 무한 재시도·잘못된 local 실행·타 target cache hit가 없어야 한다. 적합 worker 복구 뒤 동일 action 재시도가 정상 산출물만 게시하여야 한다.

## FB04 · M4의 LT/TT 부분집합 명시

**근거:** master M4 통과 gate(2136행)는 `API/TX/BR/LT/PI/TT`를 나열한다. §14.10(1106행)은 scenario blend를 M5/M6에 배치하지만 LT04(1123행)는 scenario 변경·혼합을 요구한다. §22.5(1491행)는 사용자 템플릿 저장·Linked 갱신을 M5에 배치하지만 TT05·TT07(1501/1503행)은 3-way update와 개인/프로젝트/팀 저장·버전 조회를 요구한다. MT06은 이미 1102행에서 MAT2/MAT3 부분집합을 구분하므로 같은 방식으로 정리할 수 있다.

**구체 반례:** M4 완료 시 전체 LT/TT를 요구하면 아직 M5/M6에 예정된 기능이 앞 단계의 선행 조건이 된다. 기본 bake와 Copy template만 검증한 뒤 LT04·TT05까지 PASS로 처리하면 반대로 후속 검증이 사라진다.

**수정안:** M4는 LT의 static/basic probe·unsupported 상태와 TT의 초기 유형·Copy/Preset·ID/remap·실패 복구 subset을 요구한다. scenario blend와 사용자 저장/Linked update는 M5/M6 case owner로 지정한다. 이월은 기능 삭제가 아니며 전체 suite 완료는 후속 gate에서 합산한다. M4 표에 “단계별 필수 subset”을 명시하고 각 case의 소유 단계를 별도 manifest에 연결한다.

**수용 기준:** M4 report의 `required_cases`와 `deferred_cases/owner_gate`를 분리해야 한다. 후속 사례를 PASS로 합산하지 않고, M5/M6에서도 필수성·추적 ID를 유지해야 한다. render agent의 TX/BR/MT 범위 검토 결과도 같은 원칙으로 반영한다.

## FB05 · 코드 템플릿의 신규 TypeId 발급

**근거:** master 489행 예제는 명시 TypeGuid를 사용하고 505행은 중복 TypeId를 오류로 규정한다. §22.4(1479행)는 새 asset/object ID의 clone-remap을 정의하지만 코드 선언의 타입/필드 ID는 “schema migration 규칙”으로만 연결한다. project/module/component 템플릿은 코드도 생성하므로 객체 인스턴스 복제와 다른 ID 규칙이 필요하다.

**구체 반례:** 같은 component 템플릿으로 새 C++ 클래스 DoorLogic과 WindowLogic을 각각 생성하면서 템플릿의 TypeGuid를 그대로 복사한다. 클래스 이름은 다르지만 메타 컴파일러가 TypeId 충돌을 보고한다. 반대로 prefab 복사 때 참조하는 기존 클래스의 TypeId까지 새로 발급하면 존재하지 않는 타입을 참조한다. 이 반례는 현재 생성기 실행 결과가 아니라 미정의된 계약의 두 해석이다.

**수정안:** 새 reflected/script **선언 생성**이면 declaration-local ID→새 TypeId를 최초 plan에서 1회 할당하고 적용 이력에 보존한다. 같은 plan 재시도·Linked update·rename은 그 ID를 유지한다. 기존 타입을 참조하는 asset/prefab 인스턴스 복사는 원 TypeId를 보존한다. FieldId/RpcId의 소속 범위를 명시하여 다른 새 타입의 내부 번호를 불필요하게 전역 재발급하지 않는다. 상속·외부 schema 참조와 내부 생성 선언 사이의 참조를 구분한다.

**수용 기준:** 한 템플릿에서 독립 새 타입 2개 생성→동시 meta compile이 성공하고 TypeId는 달라야 한다. 동일 plan 재시도/업데이트는 TypeId를 보존하며 이전 asset을 읽을 수 있어야 한다. 기존 클래스의 prefab 복사는 타입 ID를 변경하지 않아야 한다. TT03/04/05와 메타 중복 ID fixture를 연결한다.

## FB06 · I13의 실제 Studio/PIE 외부 조건

**근거:** detailed IT19(1078행)는 web text focus↔native viewport↔PIE 재시작이다. I13(1111행)은 IT08·IT19를 요구하고 `UiInputRouter.cpp`를 생성한다. 최신 JSON I13의 외부조건은 Windows/macOS 장비·OS/SDK뿐이다. 상위 World/UI/VM fixture 조건은 I15/I16에만 있다. 이전 DB04에서 구분한 실제 UI/PIE 조건을 최신 platform 문구와 함께 유지해야 한다.

**구체 반례:** native text session·mock route만 있는 단계에서 I13의 선행 ID와 SDK 조건은 모두 충족된다. 실제 WebView2/WKWebView·PIE host 없이 IT19 전체를 실행할 수 없으며, mock의 성공을 실제 Studio/PIE 완료로 집계할 가능성이 있다.

**수정안:** I13의 standalone composition/route-token 시험과 실제 Studio/PIE 통합 subset을 분리한다. 실제 subset에는 양OS Studio host·UI/text focus·native viewport·PIE fixture를 외부조건으로 누적한다. I15/I16은 이를 상속하고 VM/cutscene 등 자신의 추가 조건도 유지한다. 플랫폼 조건 추가가 기존 상위 fixture 조건을 대체하지 않게 생성기에서 합집합으로 구성한다. Foundation이나 I0에 Studio 완료를 선행시키지 않는다.

**수용 기준:** I13 standalone PASS와 full integration NOT RUN을 별도로 보고할 수 있어야 한다. 실제 IT19 완료는 두 OS host/PIE fixture가 존재하는 단계에서만 가능해야 한다. JSON→CSV/표 변환 후 외부조건이 소실되지 않는 계획 데이터 검사를 추가한다.

## 정합성을 확인한 사항

- Core-first, R0 최소 실행·R1 수동 schema/full STW·R2 Foundation, 병렬 GC·동적 loader의 후속 배치가 보존되었다. Foundation의 GPU·World·WebView 의존을 새로 요구하지 않는다.
- Windows x64/macOS ARM64의 명시 기준, native CI, x64/ARM64 artifact 분리, WebView2/WKWebView와 실제 viewport의 역할이 일관된다. 5개 RHI는 최초 renderer 필수이고 모든 OS가 모든 API를 실행한다는 의미는 아니다.
- Task/Core 진단은 sink/provider 주입으로 TraceAnalysis·Inspection·viewer와 분리된다. R0 logging, R1 ledger, R2 analyzer/offline viewer는 필수이며 C30은 비용 최적화만 선택이다. trace 손실·종료·metadata 수명·실제 GPU 측정의 분리가 유지된다.
- 음수 include/새 shadow header, graph cache와 action cache, runtime 참조 그래프와 cook DAG/SCC, 완전한 artifact set·늦은 output 게시 방지 규약이 존재한다.
- CPU-only·GPU offscreen·AudioNull·OfflineMixer를 구분한다. server boot/simulation M4와 16-client travel/reconnect M5를 이미 분리하였으며 이 항목은 재지적하지 않는다.
- PIE의 원본 불변·session clone·root/lease·Stop drain과 1,000회 종료 기준이 존재한다. 실제 VM/replication/GPU 사례는 해당 기능 단계에 실행하고 가짜 backend 결과와 구분해야 한다.
- 템플릿의 `rocks template` CLI, 기본 material template M3/M4, generated/user-owned 경계, 3-way update, journal 복구, shipping의 template engine 비의존이 반영되었다.
- AAA07은 격리된 검증 후보·충돌/게시 계약이 필수이고 자동 MergeBot 서비스 전체 구축은 선택이다. 관련 §27.10과 §33.6의 범위를 일관되게 읽을 수 있다.
- C31의 초기 선택 상태는 Foundation 외부 작업을 뜻한다. 전체 제품의 Rocks Build·분산/반복 최적화 필수 범위는 master §27 및 detailed §26.2에 유지되므로 그 자체를 기능 삭제로 판정하지 않았다.

## 소규모 동기화 권고

1. master §3.2(217행)의 디렉터리 예시는 여전히 `RHI/{Null,D3D12,Vulkan}`이다. 필수 backend의 계획 경로에 D3D11·Metal·OpenGL을 함께 표기한다. 기능 본문에서 이미 5종을 명시하므로 제품 요구 누락으로 재집계하지 않는다.
2. master 503행의 `GENERATED_OBJECT`를 앞선 `ROCKS_GENERATED`와 일치시키고, 1956행 `EngineServer`는 `rocks server` 또는 명시된 Rocks server binary 계약으로 통일한다. 잔존 EBT 약어는 처음에 Rocks Build의 이전 설계 약어인지 정의하거나 제품명으로 정리한다.
3. master §27.5/27.8은 Windows의 DLL/PDB/LINK 경로가 구체적이다. macOS의 native linker·symbol 산출물·bundle 서명/재시작 단계를 같은 end-to-end 계측 대상으로 한 문장 이상 명시하면 양OS 반복 개발 범위가 더 분명해진다. 상세 §4.1의 Apple Clang 계획은 이미 있으므로 macOS compiler 자체가 누락되었다고 판정하지 않는다.

위 권고와 FB01–06은 계획 문서를 구체화하는 수정이다. 실제 플랫폼 지원·성능·누수 검증의 PASS 선언으로 전환하지 않는다.

## 수정안 closure 확인

후속 확인은 FB01–06과 소규모 표기 권고에 한정하였다. 최신 작업 JSON은 **52개 작업·297개 예정 파일**이며 C00의 style 관련 4개 파일을 포함한다. 전체 재검토나 엔진 실행 검증은 수행하지 않았다.

| 항목 | 확인 결과 | 현재 근거 |
|---|---|---|
| FB01 | 표 의미 정합 PASS, 잔존 본문 참조 1곳 수정 필요 | master §1.2 정본과 detailed §16.1의 CF01–12 의미·CT 연결은 일치한다. detailed §7.3 Transform 문장의 `CF03/CT06`은 정본의 `CF04/CT06`으로 변경 필요 |
| FB02 | 문서 closure PASS | 두 문서 모두 첫 Foundation Win/Mac, Linux CPU M3·server M5 필수 후속으로 명시 |
| FB03 | 문서 closure PASS | local 완결성을 host 지원 native target으로 한정하고 외부 compiler host 부재는 CapabilityUnavailable로 실패 |
| FB04 | 문서 closure PASS | M4 표와 LT/TT 규약에 required/deferred case 및 M5/M6 owner를 명시 |
| FB05 | 문서 closure PASS | 신규 선언 TypeId 최초 plan 1회 발급·재시도/업데이트 보존, 기존 asset/prefab의 타입 참조 유지 및 TT03–05 검증 추가 |
| FB06 | 문서 closure PASS | I13 standalone/Studio subset 분리, 실제 양OS host/UI/PIE fixture 조건과 I15/I16 누적 조건을 본문·JSON에 반영 |
| 소규모 권고 | 반영 확인 | 5종 RHI 계획 경로·ROCKS_GENERATED·rocks server·Rocks Build 표기와 macOS link/symbol/sign/restart 계측 |

위 PASS는 제시한 문서 반례의 처리 규약을 확인한 결과다. 실제 구현·성능·OS 지원·누수 테스트의 PASS와 구분한다.

## 최종 closure 판정

2026-11-10 재확인: detailed §7.3의 Transform 검증 참조가 **CF04/CT06**으로 수정된 것을 현재 파일 333행에서 확인하였다. 앞선 closure 표에 남아 있던 FB01의 참조 불일치를 해소하였다.

**FB01–FB06 전체 문서 closure PASS.** 이 검토 범위의 미해결 지적은 없다. 소규모 동기화 권고의 반영 확인도 유지한다. master·상세안·작업 JSON 원본은 수정하지 않았으며, 실제 엔진 구현·OS 실행·성능·누수 시험을 통과하였다는 의미는 아니다.
