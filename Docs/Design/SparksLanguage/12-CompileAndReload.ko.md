## 12. 컴파일·핫 리로드·eval·backend

### 12.1 파이프라인과 .sparkc

`.spark` → 파서(오류 허용) → 타입 검사(binding schema·ids) → typed IR(문맥·대입·수명) → bytecode(bank 할당) → verifier → `.sparkc`

`.sparkc`는 header(format·compiler version·모듈 ID·content hash·interface hash·profile), imports(의존 interface hash, native API의 FunctionId·시그니처 hash, Tag·Action GUID), types(TypeId·FieldId·SubobjectGuid·초기값·이벤트·RpcId), functions(bank 크기·실행 문맥·await 지점별 레지스터 형태), constants, 분리 디버그 정보로 구성한다. 출력은 입력이 같으면 바이트 단위로 같다. 컴파일에 실패하면 실행 중인 게임은 마지막 성공 revision을 유지한다.

### 12.2 안정 ID

- TypeId·FieldId·SubobjectGuid·FunctionId·RpcId는 C++와 같은 `.rocksids.json` 형식을 쓴다(전체 계획 §7.1). 현재 메타 컴파일러는 빌드와 분리된 `ids add/rename/retire` 저작 명령으로만 발급하고 이름 키는 C++ 정규 이름이며 kind는 type·field·enum 계열뿐이다. Sparks 컴파일러는 자기 패키지 manifest의 **두 번째 발급자**로서 모듈 경로 기반 이름 키(`<패키지>/<경로>.<선언>`)와 subobject·function·rpc kind를 쓴다(StableIdManifest 확장, 연동 요구 SE-R8). 개발 빌드에서는 저장 후 컴파일이 새 선언의 ID를 발급하고, 입력 중인 미완성 선언에는 발급하지 않는다.
- 데이터가 연결된 ID(`@save`·`@net`·`@edit` 필드, 소유 component, enum case)의 소실은 매 컴파일의 오류이며 빠른 수정이 `@formerly("OldName")`을 삽입한다. `@formerly`는 manifest의 aliases 항목으로 기록된다. 의도한 삭제는 `rocks ids retire`(AppInfoCommandLine 14절 commandlet)로 tombstone을 남긴다. enum은 case 이름으로 저장하므로 case 이름 변경에도 `@formerly`를 쓴다.
- FunctionId는 이벤트 handler·RPC·`@cheat`·`@editorButton`·리플렉션 호출 대상·interface 구현 함수·엔진 가상 함수의 `override fn`에 발급한다.

### 12.3 증분 컴파일과 쿡

파일마다 interface hash와 함수별 body hash를 분리하고 본문 변경은 해당 함수만 다시 컴파일한다(전체 계획 §27.2·§27.3). interface hash에는 선언 시그니처뿐 아니라 `const` 값과 함수별 추론 효과 집합(§6.2)을 포함하므로, 본문 변경으로 효과가 바뀌면 호출하는 모듈이 다시 검사된다. Tag·Action·CVar 사전과 WidgetAsset schema도 그것을 쓰는 모듈의 입력이다. 쿡은 전체 컴파일·검증, 리터럴 재검증, WidgetAsset 경로 대조, 하드 의존 예산, 소프트 대상 chunk 배정, prefab·save·RPC 호환 검사, 디버그 정보 분리를 수행한다.

### 12.4 핫 리로드

핫 리로드는 필수 기능이다(사용자 요구). 편집기·개발 빌드에서 `.spark`를 저장하면 Rocks Build의 Sparks compile action(SD6)이 다시 컴파일하고, 실행 중인 모든 World(Tryout의 모든 process 포함)에 새 revision을 적용한다. 출시 빌드에는 컴파일러가 없으므로 reload가 없다(eval 정책과 별개).

**등급.** 변경마다 가장 가벼운 등급으로 처리한다(SD28).

| 등급 | 대상 변경 | 처리 | 도입 |
|---|---|---|---|
| R1 본문 교체 | 함수·handler 본문 | 새 revision을 게시한다. 이후 시작하는 handler·호출은 새 revision을 쓴다. 대기 중인 실행은 `ReloadExecutions`(아래)를 따른다 | LC7 |
| R2 제자리 migration | Sparks 필드 추가·삭제·형식 변경, `on` 선언 추가·삭제·변경, 필드 기본값, `const`, Tag·Action·CVar 사전, WidgetAsset schema | reload 경계에서 FieldId migration·구독 연결 재구성. 사전·schema 변경은 그것을 쓰는 모듈을 다시 컴파일한 뒤 적용한다 | LC7(Sparks 필드·`on`·const), 사전·schema는 EI5 |
| R3 instance 재생성 | 클래스 계층(기반 타입) 변경, `let` component 구성 변경(subobject 추가·삭제), struct 배치가 바뀌어 제자리 변환이 불가능한 경우 | 현재 필드 값을 FieldId별로 보관 → 정상 종료 hook(Disable·Destroy)과 Detach → 새 클래스로 재생성(subobject는 Prefab 3.2절 표대로, 삭제된 SubobjectGuid를 가리키던 prefab override는 Prefab 3.7절 고아 보존) → 보관한 값을 spawn parameter 단계에 덮어쓰기 → Awake·Enable. 같은 Actor graph의 재생성은 한 묶음이다. 편집 World(Tryout 밖)에 배치된 instance도 같은 경로로 재생성한다(UE reinstancing 대응) | L2 |
| R4 복제 schema 교체 | `@net` 필드 추가·삭제·형식 변경, `@rpc` 추가·삭제·시그니처 변경 | `@rpc` 추가는 RpcId manifest에 덧붙여 즉시 반영한다. 그 밖에는 클래스의 복제 schema hash를 올리고 모든 Tryout process가 같은 프레임 경계에서 새 revision을 적용한다. 삭제·변경된 RpcId의 대기 메시지는 폐기·보고하고, 서버는 해당 클래스 instance의 전체 상태를 late join 경로(§5.3 활성화 전 적용 규칙)로 다시 보내며, 클라이언트는 hash가 맞을 때까지 그 instance의 복제 수신을 보류한다 | L3 |
| Tryout 재시작 | 엔진 C++ 변경, 패키지 의존 graph 변경 | 재시작 안내(자동 재시작하지 않음) | — |

**원자적 적용과 롤백(SD29).**

1. **계획:** 컴파일·verifier가 끝나면 엔진 쪽 준비를 모두 계산한다 — 타입 revision 등록, 동적 schema, 클래스 기본값 레코드 재합성, 필드별 변환 가능 여부, subobject 변경 계획, 의존 패키지의 interface hash, 예산.
2. **판정:** 계획 단계에서 하나라도 실패하면(변환 불가가 아닌 오류, 기본값 레코드 재합성 실패, 결정성·참조 위반, 예산 초과) 아무것도 적용하지 않고 이전 revision을 유지하며 진단을 낸다. 적용 단위는 바뀐 패키지와 그 패키지에 의존하는 패키지 전체다(all-or-nothing).
3. **적용:** 실행 중인 실행 조각이 없는 reload 경계(프레임 끝 정리 뒤, §5.4 순서 7)에서 한 번에 적용한다. 디버거가 정지해 있으면 재개 후 첫 경계로 미룬다.
4. **instance 단위 실패:** 적용 중 R3 재생성의 Awake·Enable fault처럼 instance 하나에서 난 실패는 그 instance만 격리하고 나머지는 계속한다.

**보존 규칙.**

- 필드 값은 FieldId로 보존한다. 형식이 바뀌면 저장 로드와 같은 무손실 변환(§15)을 쓰고, 불가능하면 기본값을 두고 보고한다. enum은 case 이름으로 다시 대응시키며 삭제된 case는 첫 case로 두고 보고한다. struct·컨테이너 원소는 FieldId 단위로 재귀 migration한다.
- 객체 handle·로드한 에셋 값·유지 자원 필드는 형식이 같으면 그대로 두고, 다르면 해제한다. 삭제된 필드의 유지 자원·lease는 해제한다.
- 새 기본값은 spawn 이후 한 번도 쓰지 않은 필드(필드별 "쓰기 있음" bit)에만 반영하고, 쓴 필드는 편집기에 표시한다. prefab·instance override가 있는 필드는 override를 유지한다.
- reload가 적용한 값은 Changed를 발행하지 않는다. 구독 연결 순서는 유지되며 새 `on` 선언은 끝에 추가된다.
- Tick 설정(`@tick`)이 바뀌면 엔진 `TickSettings`를 다시 등록한다(엔진은 OnStart 때 한 번 등록하므로 bridge가 해제·재등록한다).
- Tick fault로 중지된 Tick은 그 클래스 모듈의 새 revision 게시 때 다시 시작한다. 격리된 instance는 개발 빌드에서 "이 revision으로 재생성" 명령으로 R3 경로를 밟을 수 있다(§8.4).

**대기 중인 실행(SD30).** 실행을 새 코드로 자동 이전하지 않는다(§20.3 #6). 프로젝트 설정 `ReloadExecutions: Keep | Cancel`이 정하며 Tryout 기본값은 `Cancel`이다.

- `Keep`: 대기 중인 실행은 이전 revision으로 끝까지 실행하고 그 실행이 하는 호출도 이전 revision 안에서 해석한다(전체 계획 §23.4).
- `Cancel`: 본문이 바뀐 함수를 실행 스택에 가진 실행을 취소한다. 취소된 Start는 처음부터 다시 실행하고, 이벤트 handler는 다음 이벤트를 기다린다. `while` + `await NextTick()` 반복이 이전 코드로 계속 도는 일을 막는다.
- R2·R3에서 변경·삭제된 FieldId를 bytecode에서 참조하거나 삭제된 handler에 속한 실행은 설정과 무관하게 취소한다.
- 모듈별 살아 있는 revision은 8개까지다. 넘으면 가장 오래된 revision의 실행을 취소하고 보고한다.

**편집기 표시.** reload마다 요약을 보인다: 적용된 revision, 등급별 대상 수, 취소된 실행 수, 해제된 자원, 손실이 있었던 변환, 재생성·격리된 instance, 다시 시작한 Tick, 이전 revision으로 실행 중인 handler 수와 재시작 명령. 실패한 reload는 원인 진단과 "이전 revision 유지 중"을 표시한다.

**엔진 쪽 계약(연동 요구 SE-O2·O3·A3·W1·W5).** 같은 TypeId 아래 여러 revision 레코드의 공존과 instance별 revision 포인터·경계에서의 교체·마지막 lease 반환 시 retire(RCM "모듈 reload"의 Busy 거부를 대체), 스크립트 필드의 동적 schema 등록과 graph encode/decode·override 경로·Changed/CU5 통지, live instance의 subobject 추가·삭제, World 프레임의 reload 경계 hook.

**목표와 시험.** 본문 reload는 저장부터 적용까지 2 s 이내(SD8, 전체 계획 I1). G2 soak의 "5분마다 reload"는 L2 Gate, G5 reload 행렬(§9.3)은 등급별 L1–L3 Gate다.

탈출하는 상수 문자열은 참조계수 버퍼로 복사하여 revision을 고정하지 않는다.

### 12.5 스크립팅 backend: VM과 native AOT

스크립팅 backend는 Unity의 Mono/IL2CPP 선택과 같은 맥락의 **옵션**이다(SD31). AOT 기능 자체는 반드시 제공하지만 쓸지 말지는 target·빌드 구성마다 프로젝트가 고르며 강제 기본값이 없다.

| 설정 | 값 | 기본 | Unity 대응 |
|---|---|---|---|
| `ScriptingBackend`(target × 빌드 구성) | `Vm` · `Aot` | `Vm` | Player Settings의 Scripting Backend(Mono/IL2CPP) |
| `AotOptimization` | `Debug` · `Release` · `Master` | `Release` | IL2CPP의 C++ Compiler Configuration |
| `AotFallbackModules` | module 목록 | 비어 있음 | — |

- 기본값이 모든 target에서 `Vm`인 이유: Rocks VM은 interpreter라 실행 가능한 메모리를 할당하지 않으므로(§12.6) iOS·콘솔에서도 강제할 이유가 없다. 두 backend는 관찰 가능한 결과가 같으므로 선택은 성능·실행 파일 크기·빌드 시간의 trade-off다.
- 편집기와 Tryout은 항상 VM으로 실행한다(핫 리로드). 개발 빌드에서 `Aot`를 고르면 아래 혼합 규칙으로 reload를 유지한다.
- bytecode(`.sparkc`)는 backend와 무관하게 항상 생성하고, **VM interpreter는 AOT 구성에도 항상 링크한다** — 출시 후 콘텐츠 패치·DLC의 새 스크립트, eval·외부 모듈(항상 VM), 로드 시 hash 불일치 fallback에 필요하다.
- 파이프라인은 typed IR → C++ 생성 → 플랫폼 toolchain이다(전체 계획 §23.1의 "LLVM 기반 AOT"와 다르므로 §17에 기재).[R8][R9] JIT가 아니므로 iOS·콘솔의 JIT 제한과 무관하다.[R5]

**두 backend의 동등성 규칙.** AOT 생성기는 다음을 지켜 VM과 비트 단위로 같은 결과·fault·이벤트 순서를 낸다. 검증은 G6 차등 시험(§9.3)이다.

1. **예산 계수:** basic block마다 그 block에 대응하는 bytecode 명령어 수를 더하고 VM과 같은 지점(back-edge·호출 복귀)에서 검사한다. 호출 깊이·메모리 예산도 같은 논리 단위다.
2. **평가 순서:** 모든 부분식을 임시 변수로 순서화해 왼쪽에서 오른쪽 평가(§3.3)를 지키고, `&&`·`||`·`??`·`?.`는 명시적 분기로 생성한다.
3. **부동소수점:** strict FP, fast-math·재결합·FMA 축약 금지, `long double` 미사용, float32 변환은 최근접 짝수, `-0.0` 보존. 초월 함수는 두 backend가 같은 Core Math 함수(같은 실행 파일의 같은 심볼)를 호출하며 AOT가 컴파일러 내장 함수로 바꾸지 않게 한다. 컴파일러별 FP 설정은 build 규칙으로 강제한다.
4. **정수:** overflow 검사 intrinsic, `Math.Div`·`%`·int64 최솟값은 공통 runtime 함수.
5. **필드 쓰기·이벤트:** VM opcode와 같은 runtime 경로(Changed 후보 기록, emit 큐, 파괴·활성 전환 지연)를 호출한다. 필드 직접 대입으로의 최적화는 금지한다.
6. **safepoint·취소:** await·back-edge·호출 복귀·할당 위치를 VM과 1:1로 맞추고 generation 확인도 같은 지점에서 한다.
7. **위치 ID:** fault 보고 key의 위치는 bytecode 위치 ID로 통일하고 AOT는 검사 지점마다 이 ID를 상수로 넣는다. 보고·집계·격리 판정이 backend와 무관해진다.
8. **native stack:** AOT frame이 커서 호출 깊이 예산보다 먼저 native stack이 넘칠 수 있으므로 함수 진입마다 남은 stack을 검사하고 부족하면 `BudgetExceeded`로 처리한다(process를 끝내지 않음).

**AOT를 골랐을 때.**

| 기능 | VM | AOT | IL2CPP와 비교 |
|---|---|---|---|
| 런타임 eval·외부 모듈 | eval 정책을 따름 | VM 경로로 그대로 동작 | IL2CPP는 런타임 코드 로드 불가 |
| 핫 리로드(개발) | 지원 | 지원 — 고친 함수만 bytecode revision으로 교체 | IL2CPP는 불가 |
| 디버깅(개발) | DAP 전체 | breakpoint를 건 함수는 VM 진입으로 교체(deopt), 정지한 AOT 실행은 함수 단위 breakpoint만. native 디버거는 `#line`으로 `.spark` 줄을 표시 | development build 스크립트 디버깅 |
| 출시 진단 | 위치 보고 | 크래시·fault 위치 역변환 | 같음 |
| 리플렉션 호출 | 가능 | FunctionId 발급 대상(§12.2)은 strip하지 않는다. 사용자 generic이 없어 generic sharing 문제가 없다 | managed stripping |
| 빌드 시간·크기 | 짧음·작음 | 길음·큼(§11 측정 항목) | 같음 |

**VM과 AOT 혼합.**

- FunctionId마다 진입 간접 표(AOT 진입 또는 VM 진입)를 둔다. 개발 구성에서는 모든 Sparks→Sparks 호출이 이 표를 거치고 Sparks 함수끼리의 C++ inline을 금지한다. 출시 구성은 inline을 허용하되 위치 ID와 예산 계수 의미를 유지한다.
- 개발 구성 AOT는 필드 접근을 binding 표를 거쳐 하도록 생성한다. reload로 필드 배치가 바뀌면 그 클래스를 쓰는 AOT 함수를 VM 진입으로 바꾼다.
- 대기 중인 AOT 실행은 §12.4의 `ReloadExecutions`를 따른다. AOT 코드는 실행 파일에 정적으로 있으므로 revision lease가 반환되어도 unload할 것이 없다.
- 호출 규약: 경계에서는 VM frame의 bank 창(Scalar·Ref·Buf)으로 인자와 반환값을 주고받고 AOT 내부만 native 인자를 쓴다. Ref가 GC root가 아니므로 stack map이 필요 없다.
- async 함수·handler는 명시적 상태 기계 struct로 생성하고 ledger에 집계되는 실행 allocator에서 할당한다.

**빌드.**

- module마다 `.cpp` 1개를 생성하고(크면 분할) unity build·PCH를 쓸 수 있게 한다. 입력 key는 (module content hash, 의존 interface hash, binding hash, 생성기 버전, `AotOptimization`)이며 같으면 Rocks Build action 결과를 재사용한다.
- 시간·경로·순회 순서에 의존하지 않고 이름은 TypeId·FunctionId로 만들어 "입력이 같으면 바이트 동일"(§12.1)을 생성 C++에도 적용한다.
- 생성 C++에 `#line "<pkg>/X.spark" N`을 넣고, 분리된 symbol에 (native 주소 → module hash, 함수, 위치 ID) 표를 두어 VM과 같은 형식으로 오프라인 역변환한다(§8.3).
- 생성 코드는 예외·RTTI·STL 없이 엔진 runtime 헤더만 쓰고 같은 policy 검사를 받으며 주석은 영어다. 모든 플랫폼 toolchain의 공통 부분집합으로 쓴다.
- verifier는 생성 시점에 실행한다. 로드 시 SD7 검증은 AOT 코드에 대해 (module content hash, FunctionId·시그니처 hash, binding hash) 대조로 한다.

**실패와 fallback.**

- 런타임: `.sparkc`의 content hash가 실행 파일 안 AOT 코드의 hash와 다르면 그 module은 VM으로 실행하고 1회 보고한다(오류 아님).
- 빌드: AOT를 고른 구성에서 생성·C++ 컴파일이 실패하면 빌드 실패다. `AotFallbackModules`에 명시한 module만 VM fallback을 허용하고 빌드 결과에 표시한다. 차등 시험 불일치는 fallback으로 덮지 않는다(Gate 실패).
- 개발 중: AOT 생성이 실패하면 진단을 내고 VM으로 계속한다. 핫 리로드는 그대로다.

### 12.6 런타임 eval 정책

| target 정책 | 값 | 출시 기본(SD4) |
|---|---|---|
| `ScriptEval`(소스 eval) | `Off` · `DevOnly` · `On` | `Off` |
| `ExternalModules`(외부 모듈) | `Off` · `DevOnly` · `Signed` · `On` | `Off` |

두 정책은 빌드 상수다. `Off`이면 해당 기능을 링크하지 않고, `DevOnly`·`On` 안에서 실제로 켜는 것은 `BootCVar`(`sparks.eval.enabled`, `sparks.modules.enabled`)다. 스크립팅 backend 선택(§12.5)과는 독립이다 — AOT 구성에서도 eval·외부 모듈은 VM으로 실행한다.

`Off` target에는 컴파일러를 링크하지 않는다. JIT가 아니므로 실행 가능한 메모리를 할당하지 않으며, 마켓 판단 대상은 다운로드한 코드로 기능을 변경하는 것을 제한하는 규정이다(App Store 2.5.2).[R31] 콘솔 규정은 공개 자료로 확인하지 못하였다.

### 12.7 eval·외부 모듈의 수명

- **source eval**은 기존 타입에 대해 함수·handler를 실행하며 타입을 선언하지 않고, 변경 가능한 전역이 없고, 이름을 새로 등록하지 않는다. 같은 정적 타입 검사·검증·예산·ledger를 거치며 호출 가능한 API는 capability(기본 `ModSafe`: World 읽기·쓰기, IO·네트워크·저장 금지)로 제한한다.
- **외부 모듈**은 자체 ID manifest를 갖고 타입을 pool과 무관한 NameKey([Core 상세 사양](../CoreDetailedSpec.ko.md) T02)로 등록한다. 의존 패키지의 interface hash가 다르면 로드를 거부한다. `@save`·`@net`·`@rpc`는 서명된 모듈에서만 허용한다.
- **서명:** 쿡 패키지 안의 `.sparkc`는 콘텐츠 서명(RSIG)이 이미 덮는다. 외부 모듈 서명은 RSIG 형식을 재사용하되 label `"rocks.sparks-module.v1\0"`, 모드 서명 전용 키 목록, `contentVersion` 하한과 `scope`를 갖는 별도 봉투다. Release가 매니페스트 밖 입력을 읽지 않는다는 ContentSigning 1절에 `ExternalModules=Signed` 경로를 예외로 등록하고, `ScriptEval=On`인 출시는 매니페스트 밖 입력을 받는다는 점을 target 결정 기록에 남긴다(연동 요구 SE-B6).
- 외부 모듈이 생성한 Actor의 수명은 World가 관리하며 모듈 unload 전에 파괴한다. 저장 대상에서 제외(transient)한다.
- unload는 비동기다(Unloading → Unloaded). TypeRegistry가 Busy이면 120 프레임 동안 재시도한 뒤 그 모듈을 사용 중지하고 Gate 실패로 보고한다. Unloading 중 호출은 `ModuleUnloading` 오류를 반환하고, `Result`를 반환하지 않는 호출은 `ContractViolation` fault다.

### 12.8 신뢰 경계

| 입력 | 신뢰 | 검사 |
|---|---|---|
| 빌드에 포함된 `.sparkc`·AOT 코드 | 신뢰 | RSIG(쿡 패키지), 로드 시 verifier 또는 hash 대조(SD7) |
| 서명된 외부 모듈 | 조건부 | 모듈 서명 봉투, verifier, capability, 예산 |
| 서명 없는 외부 모듈·source eval | 비신뢰 | verifier(외부 bytecode fuzz Gate 대상), `ModSafe` capability(World 읽기·쓰기만, IO·네트워크·저장·`@cheat`·편집기 API 금지), 예산(DoS 방어), session 격리 |
| 저장 파일 | 비신뢰 | 길이·값·범위 검사, placeholder 로드(§15) |
| RPC 인자 | 비신뢰 | interest·소유 connection·길이·유한성·enum 검사, 빈도 제한(§14) |

로그에는 민감 정보(계정·토큰·결제 정보)를 쓰지 않는다(LoggingSystem 12절). verifier는 보안 경계이므로 외부 bytecode를 받는 target에서는 fuzz Gate를 통과해야 한다.
