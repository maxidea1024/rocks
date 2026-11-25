## 17. 스펙 개정 대상

### 17.1 전체 계획·입력 상세안·UI 툴킷(초안 1.2까지 반영)

개정 대상은 전체 계획, 입력 상세안, UI 툴킷, Core 구현 코드다. 위치 열의 절 번호는 접두어가 없으면 전체 계획의 절이다. "현재 문구"는 전체 계획 개정 1.6 기준이며(개정 1.8 행은 1.7 기준), 문서 항목은 모두 전체 계획 개정 1.7·1.8, 입력 상세안, UI 툴킷에 반영하였다. 수명 단계 이름(BeginPlay 등)을 쓰는 전체 계획 §5.3과 입력 상세안 586행은 엔진 내부 단계 이름이므로 그대로 두고 전체 계획 §20.2의 대응표로 연결한다. Core 구현 코드 C1–C4는 연동 단계 EI3의 선행이다(연동 요구 SE-C1–C4, C5는 구현됨).

| 위치 | 현재 문구 | 이 설계 |
|---|---|---|
| §23.1 이름·예시·범위 | 이름 미정, `ObjectRef`·`WeakObjectRef`, Result·generic·interface, 동적 eval 제외, LLVM 기반 AOT | Sparks, `T?` 단일 개념, 사용자 generic·closure 제외, interface full, eval target 정책, 스크립팅 backend 선택(VM 또는 typed IR → C++ AOT, SD31) |
| §23.2 VM·GC·coroutine | stack depth 검사, coroutine root 열거, 메모리 예산 | bank 검사, 스크립트 상태는 GC root 없음(약한 handle·lease), 논리 단위 메모리 예산, 구조적 동시성 |
| §23.1·§27.8 VM 방식·reload 목표 | VM 방식 미정, reload 시간 목표 | 레지스터 VM과 Scalar·Ref·Buf bank, reload 목표 I1 2 s(SD8) |
| §5.2·§20.2 파괴·수명 | RequestDestroy 즉시 전이, 수명은 component 기준, hook 이름 BeginPlay 등 | 스크립트 단계 중 파괴는 단계 끝 반영(SD3), 수명 hook을 service·widget controller에도 적용, Unity 체계 hook 이름과 C++ `On*` 가상 함수(SD9), BeginDestroy에서 Detach |
| §5.3, 입력 상세안 586행 | "optional integration Register / BeginPlay" | hook 대응표(§5.1)와 연결 |
| §23.2(§8.3 clock 대응) | Monotonic·Frame·Game·Fixed·UI | `Delay(s, Clock.Game\|Clock.UI)`, `NextTick`(가변 tick, 일시 정지 중 미재개), `NextFixedTick`(Fixed) |
| §9.2·§10.2·§10.3·§24.2 로딩 | LoadTicket·AssetLease·revoke·travel·bundle | 요청 handle·결과 값 보관, 대기자 분배, Spawn의 prefab lease, 외형 전용 stub, 필드 초기값 에셋은 prefab 의존, 엔진 관리 로딩 화면, Teleport 내부 prefetch |
| §23.4 native API 선언 | lane·최대 작업·취소 | + 스레드 안전성·결정성·부수 효과·MaySafepoint·권한·자원 정책·파괴 지연·고립 완료 경로 |
| §29.6 NL03·NL04·NL06 | Tryout 1,000회 등 | NL09 신설(G1–G4 Sparks Gate), 무기한 재시도 대신 모듈 사용 중지·실패 보고 |
| UiToolkit CU2·CU3·CU5·CU8, CU9 신설 | UI 사용처 중심 | 언어 L1 선행. CU3은 `MulticastDelegate` 필드와 `FieldKind::Event`. CU9는 §10.3에서 정의 |
| 입력 상세안 §22.3 | subscription이 module/script lease 보유, VM reload 시 해제 | 연결 항목은 handler ID만 보유, Disable은 비활성 표시, 선언이 바뀌는 reload에서만 재구성 |
| Core `Delegate.h`·`NamePool.h`(구현) | 약한 바인딩은 `SharedPtr`만, TryAdd compact 없음 | C1–C4(C5 `TryFind`는 구현됨) |
| §23.3·§2.4 | Studio language service, 언어 경계 명명 보류 | LSP·DAP, 패키지 `Rocks`, 필드 첫 글자 대문자 변환 |
| §2.4 C++ 오버로드(개정 1.8) | 서로 다른 `ScriptName`이 있어야 노출 | 노출 함수의 오버로드에 서로 다른 `ScriptName`이 없으면 메타 컴파일 오류 |
| §5.2·§20.2·§23.2(개정 1.8) | 스크립트가 요청한 파괴만 지연, 연쇄 단계당 16회, Enable·Start fault는 instance 비활성화 | 스크립트 단계 동안의 모든 파괴 요청 지연, 인과 깊이 16, 격리, 종료 경로별 hook(SD20), 재진입 정책 표시(SD11) |
| §22.6 session 구성(개정 1.8) | VM globals·coroutine·root | 스크립트 실행·연결 표·대기(전역 변수와 GC root 없음) |
| §23.4 conformance(개정 1.8) | 순환 import를 오류 사례로 열거 | 패키지 간 순환 의존(파일 간 순환 참조는 허용) |

### 17.2 다른 문서의 후속 개정

엔진·도구 **코드**에 필요한 사항은 [Sparks 엔진 연동 요구 목록](../SparksEngineIntegration.ko.md)이 정본이다(초안 1.5에서 분리). 항목마다 현재 상태와 그것이 필요한 연동 단계를 적고, 엔진이 준비되는 대로 하나씩 더한다. 이 절에는 다른 **문서**의 개정만 남긴다.

| 대상 | 필요한 변경 | 시점 |
|---|---|---|
| 전체 계획 §5.2·§9.2·§20.2–20.5·§23 | Entity → Actor, TransformComponent → SceneComponent, `OnAwake(ComponentContext&)` → 현재 시그니처, tick group 이름을 World tick phase로(ER 2절이 예고한 개정의 미반영분), §23의 도입 단계표를 언어 코어·엔진 연동 두 갈래(§18)로, "LLVM 기반 AOT"를 스크립팅 backend 선택(SD31)으로, §29.6 NL09에 G5·G6 추가 | 다음 전체 계획 개정 |
| ER 7.2·7.4·8절 | Component 시작 준비 hook과 Start bridge 계약(SD24), 파괴·활성 bridge 큐와 격리 bridge 계약(SD25, §8.3), 스크립트 단계 hook(§5.4) — 연동 요구 목록 SE-W1–W3의 설계 | EI3 전 |
| ObjectFoundations·GC 대규모 처리 | 회수 전 cleanup hook(SE-O1)의 설계 | EI2 전 |
| RCM·TypeRegistry | 타입 revision과 스크립트 클래스 등록(SE-O2·O3)의 설계 | EI2 전 |
| Prefab | Sparks reload(R3)에 따른 subobject 표 변경 행, live instance의 subobject 추가·삭제 | EI6 전 |
| UiToolkit CU1–CU9 | CU9 규격(§10.3) 반영, CU1 attribute 대응(§10.1), CU5 계산 속성 setter 통지(§10.2) | EI1 전 |
| CompilerDiagnostics | 엔진 공통 진단 코드 등록부(Sparks S0100–S1199, 메타 컴파일러 RMC####, UI validate 코드) | LC0와 함께 |
| ContentSigning 1절 | `ExternalModules=Signed` 예외와 모듈 서명 봉투(§12.7) | EI9 전 |
| CVarSystem 14.5절 | Sparks 노출 문법(§10.4) 반영 | 다음 CVar 문서 개정 |
| 게임 타입 자리표시 | `Character`·`Collider`·`SphereCollider`·`SoundAsset`·`Sequence`·`DataAsset`·`ScriptableComponent`·`WidgetController`·`PlaySession`의 Rocks 이름 확정(Rocks 고유 이름 원칙) | 해당 모듈 진입 시 |
| ID 개념 이름 | `SoftEntityId`·영속 Entity Guid·파생 Entity ID를 Actor 계열 이름으로 바꿀지 Prefab과 함께 결정 | Prefab PF 단계 |
| (초안 1.4) 편집기 | Outliner "레벨 스크립트" 항목, 레벨 스크립트 명령·템플릿(전체 계획 §22.3), `sparks-lsp` 명령 `sparks.levelScript.*`(§13) | 편집기 단계 |

끝난 항목: 코드의 `WeakRef<T>` → `WeakObjectRef<T>` 개명(커밋 `e0e78953`, SD23), Core C5 `NamePool::TryFind`.
