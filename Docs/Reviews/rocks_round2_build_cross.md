# Rocks 구현 계획 Round 2 교차 검토 — 빌드·작업 순서·기능 연결

검토일: 2026-11-10. 대상: `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `Docs/Design/CoreObjectInputData.json`.

기준 시점: 아래 인용과 행 번호는 root의 Round 2 일괄 수정 **직전** 원본에 해당한다. 보고서 작성 도중 root가 해당 수정 반영을 알렸으므로, 현재 파일에 남은 결함이라는 뜻으로 읽지 않는다. 수정 후 판정은 별도 `Docs/Reviews/rocks_round3_build_closure.md`에서 기록한다.

## 판정과 범위

새로 확인한 수정 항목은 **P2 2건**이다. 하나는 R0의 최소 동기화 계약과 예정 파일 최초 도입 시점의 불일치이고, 다른 하나는 generated 코드의 새 formatter 정책을 캐시 입력 계약에 구체적으로 연결하지 않은 점이다. 첫 Foundation 구현에 착수하기 전에 작업 데이터와 검증 기준에 반영하는 편이 적절하다.

이 보고서는 문서의 선행 조건과 실패 반례를 검토한 결과다. 엔진 소스의 컴파일, 캐시 실행, 플랫폼 장치 시험을 수행하였다는 의미가 아니다. 아래 PASS는 제시한 복합 시나리오에서 **새로운 확정 모순을 발견하지 않았음**을 뜻하며 구현 통과 판정은 아니다. 기존 FB01–06에서 닫힌 문제는 신규 결함으로 다시 세지 않았다.

## R2B01 — R0 mutex 사용보다 늦은 공개 Sync 계약 도입

**우선순위: P2 / 구현 순서와 파일 계획 보정**

**위치와 근거**

- 상세안 5.1절, 225행: “Windows/Mac의 page·clock·atomic·sync는 C02부터, thread·join은 C16”이라고 명시한다.
- 상세안 15.1절, 645행: “C02에는 Registry에 필요한 atomic과 mutex의 최소 구현을 포함하고, C16에서 thread·condition·wait와 memory-order 검증을 확장한다.”
- 상세안 15.1절 작업표 671행은 C02를 R0, 685행은 C16을 R2-A로 배치한다.
- JSON `tasks[ID=C02].파일목록`에는 Win64/Mac의 `Sync.cpp`가 있지만 공개 `Engine/PlatformBase/Public/Platform/Sync.h`는 없다. 같은 파일은 `tasks[ID=C16].파일목록`과 `files`의 C16 소유 산출물로 처음 나타난다(JSON 302행 및 2067행).

**반례**

작업 목록에 따라 C00부터 C12까지의 R0/R1 파일만 도입한다. C02의 mutex 구현과 R1 Registry의 gate는 공통 공개 선언을 사용해야 하지만 그 계약 파일은 R2-A의 C16에 배치되어 있다. 구현자가 파일 계획을 그대로 지키면 나중 단계의 파일을 먼저 추가하거나 Registry 쪽에 별도 OS 선언을 중복해야 한다. 전자는 작업 완료 증거와 파일 소유 시점을 어긋나게 하고 후자는 PlatformBase 경계를 약화한다. 이는 실제 컴파일 실패를 재현한 결과가 아니라 **문서가 정한 단계별 산출물을 엄격하게 적용했을 때 나타나는 선행 조건 누락**이다.

**최소 수정**

1. 최소 `Sync.h`의 최초 도입과 파일 소유 작업을 C02로 이동한다. C02는 mutex 생성·해제·lock·unlock, 최소 상태/오류 의미만 제공한다.
2. 해당 공개 계약은 EngineBase의 POD·OS 상태 계약만 사용한다. 아직 없는 Core의 String/Result, Task worker, loader를 요구하지 않는다.
3. C16은 같은 동기화 계약에 condition·wait·thread lifecycle 검증을 확장한다고 명시한다. 최초 생성 파일 목록에는 중복 등록하지 않는다.
4. C02의 CT07 부분 검증과 C16의 전체 동시성 검증을 구분한다. 실제 thread/join 시험의 성공을 C02 완료 조건으로 앞당기지 않는다.

**수용 기준**

- C00–C12 산출물만 존재하는 Win/Mac Foundation fixture에서 공개 Sync 헤더를 포함한 최소 mutex/Registry gate 시험을 만들 수 있다.
- 공개 Sync 헤더는 EngineBase만으로 선언을 해석할 수 있고 C16 파일이나 상위 World/VM/Task worker를 include하지 않는다.
- 예정 파일 목록에서 `Sync.h`의 최초 도입 소유자는 C02 하나이며 C16의 확장 책임을 별도로 기록한다.
- CT07 결과는 C02의 최소 primitive와 C16의 thread/wait 확장 구간을 구별해 출력한다. 작업 ID DAG 검사와 함께 단계별 파일 도입 순서도 확인한다.

## R2B02 — formatter 정책과 생성 캐시 입력·CT12의 연결 누락

**우선순위: P2 / 새 정책의 캐시 정확성 계약 보강**

**위치와 근거**

- 상세안 4.3절 197행은 formatter 실행 파일과 `.clang-format`의 고정을 요구한다. 199행은 “generated 파일은 생성 단계부터 같은 설정으로 출력한다”와 PR의 `clang-format --dry-run --Werror` 검사를 명시한다. 마스터 3.4절 299–301행에도 같은 정책이 있다.
- 상세안 9.3절 440행의 구체적인 생성 캐시 입력은 “생성기·schema·include closure·target ABI”다.
- JSON C20은 AST schema/trace 생성과 재생성 최소화, CT10/CT12를 담당하지만 formatter 설정만 바꾼 캐시 무효화 반례는 아직 명시하지 않는다.
- 마스터 27.2절 1963행의 일반 tool binary/input 완전성 원칙은 이미 올바르다. 따라서 이 항목은 기존 full-input 원칙 자체의 결함이 아니라, **추가한 formatter 정책이 구체적인 codegen action 입력으로 누락되지 않도록 연결하는 보강**이다.

**반례**

한 번 생성한 헤더/cpp를 캐시에 저장한 뒤 C++ 원본·AST·schema·target ABI는 그대로 두고 `.clang-format`의 `SeparateDefinitionBlocks` 또는 열 제한만 변경한다. 또는 고정 formatter 실행 파일 버전만 변경한다. C20의 구체 열거 항목만 구현하면 예전 formatted 출력이 hit로 복원될 수 있다. 그러면 현재 서식 규칙의 CI 검사가 실패하거나 clean build와 warm cache의 최종 generated 파일이 달라진다. C++ 의미가 같더라도 문서가 요구한 재현 가능한 산출물과 동일 서식 정책에는 맞지 않는다.

**최소 수정**

1. codegen action의 입력 closure에 formatter 실행 파일의 digest/버전, 최종 해석된 서식 설정 내용의 digest, formatting flag/mode를 포함한다. 설정을 찾는 위치나 상속이 출력에 영향을 주면 해석에 필요한 입력도 포함한다.
2. `generate + format`을 하나의 action으로 만들거나, raw 생성 출력 → FormatAction → 최종 generated 출력의 명시적 DAG로 구분한다. 최종 파일 producer는 하나만 둔다.
3. 컴파일 action은 최종 formatted 출력의 내용 hash를 입력으로 사용한다. action graph 밖에서 formatter가 파일을 다시 쓰는 방식은 사용하지 않는다.
4. formatter 실패·취소는 성공 artifact를 publish하지 않는다. 최종 바이트가 같으면 timestamp를 바꾸지 않는다. C20과 CT12의 상세 기준 및 작업 데이터에 이 반례를 추가한다.

**수용 기준**

- warm cache 상태에서 formatter 설정만 변경하거나 formatter binary만 변경하면 영향을 받는 action의 hit가 무효화된다.
- clean/warm 및 local/remote 실행의 최종 formatted generated 파일이 바이트 단위로 일치한다. 비교 대상에는 설정 변경 후의 산출물을 포함한다.
- formatter와 설정이 모두 같은 no-op 실행은 재생성/재포맷을 수행하지 않으며, 실제 최종 바이트가 그대로이면 downstream 재컴파일도 만들지 않는다.
- formatter 실패 시 이전 출력은 last-good 진단용으로 보존할 수 있지만 새 빌드의 성공 결과로 인정하지 않는다. 한 output의 이중 producer와 action 밖의 출력 변조를 negative test로 거부한다.

## 복합 시나리오 대조

| 시나리오 | 연결한 기능·작업·검증 | 반례 대입과 판정 |
|---|---|---|
| S01 첫 Core 구현 | C00 scaffold → C02 Platform primitive → C06 callback/lease → R1 manual schema·Registry·full STW GC | Clang/World/VM/C16 없이 첫 구현을 시작하는 흐름은 의도상 성립한다. 다만 공개 mutex 계약의 최초 파일 소유가 늦어 **R2B01 수정 필요**다. |
| S02 타입 선언부터 자체 언어까지 | 상세 7.3·9절·15.1, 마스터 23절: C01 Guid → manual schema → C19/C20 generated schema → C23 ObjectSerialization bridge → M3 L0/L1 native binding | 새 컴포넌트 템플릿이 타입을 선언하고 나중에 자체 언어에서 참조하는 경로를 대입하였다. Core가 VM/Clang 결과를 먼저 요구하는 새로운 순환은 확인하지 못하였다. C++ native schema를 먼저 만들고 VM이 이를 소비하는 순서를 지키는 조건으로 **문서 PASS**다. |
| S03 머티리얼·UI·스크립트 제작 | 마스터 14.6·21·23절, 상세 26.1: MaterialSchema/Compiler/Runtime/Editor 분리 → M3/M4 기본 제작 → M4 callback → M5/M6 binding·layer 확장 | 기본 UI/Material 편집 완성을 이유로 뒤 단계의 복합 binding/layer 지원을 완료로 보고하는 경우를 대입하였다. 초기와 후속 Gate가 구분되어 있고 unsupported target을 성공으로 대체하지 않는다. 상위 제작 기능을 C00–C26 Foundation 선행 조건으로 강제하는 새 연결도 없어 **문서 PASS**다. |
| S04 시간·입력·PIE 종료 | 상세 18.2·21.2·22·23절: pause/time dilation/catch-up → ActionEventBatch → UI/IME focus → weak script callback → semantic replay → PIE Stop | 한 OS 입력의 중복 소비, pause 중 UI 정지, 이전 route/session의 callback, replay 시 OS 입력 재독출을 대입하였다. unscaled UI 시간, 이벤트 전이 보존, route/context token, semantic replay와 side-effect ledger가 각 경계를 맡는다. fake Core context 시험과 실제 World/VM 통합시험은 별도다. 기존 계약 범위에서 **새 확정 모순 없음**이다. |
| S05 OS·headless·실제 장치 Gate | 마스터 2.5·28·30절, 상세 24.3·26절: Win/Mac Foundation → Linux CPU/server 필수 후속 → macOS Studio → CPU bake → 없는 cross worker | Core/CPU target 성공을 GPU 렌더·full Studio 성공으로 승격하는 경우와 없는 foreign compiler를 local fallback 성공으로 표시하는 경우를 대입하였다. 필요한 capability와 실제 fixture 조건이 별도로 남는다. CPU bake도 material profile 조건을 가진다. 이 영역의 기존 FB closure를 재검토한 것으로 **신규 결함으로 재집계하지 않는다**. |
| S06 코드 생성과 캐시 | C20/CT12 → generated 파일 서식 → shared cache → CI 포맷 검사·증분 컴파일 | source/schema가 같은 상태에서 formatter 설정/실행 파일만 교체하는 반례는 현재 구체적인 생성 입력 목록에서 놓칠 수 있다. **R2B02 수정 필요**다. generic full-input 원칙은 유지하고 구체 입력과 negative test를 추가하면 된다. |

## 수정 및 다음 검토의 경계

두 항목은 마스터의 기능 범위나 Core-first 우선순위를 바꾸지 않는다. 원본에는 손대지 않았다. R2B01은 C02/C16 파일 소유·부분 검증을, R2B02는 마스터 action 입력 계약·상세 C20/CT12·작업 데이터의 검증 항목을 함께 고치면 된다. Round 3에서는 이 두 반례가 실제 수정 문구와 데이터에서 닫혔는지 확인하고, 문서의 설계 검토 결과와 아직 수행하지 않은 엔진 구현 검증을 계속 구분한다.
