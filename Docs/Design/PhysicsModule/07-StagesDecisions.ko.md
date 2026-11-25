# 물리 모듈 — 7. 단계·시험·결정

[목차](README.ko.md)

## 7.1 단계

모두 M4 물리 단계에서 계획 순서대로 한다(사용자 결정 2026-11-25 — 스펙만 지금). 같은 날 화면 마일스톤 뒤라 PH1부터 착수했다(7.1.1절). 묶음마다 설계 갱신 → 구현 → 리뷰어 1명 → 커밋.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| PH0 | 이 문서 | JL1·CD1·CD2 | 리뷰어 1명 반영 |
| PH1 | `Physics` 모듈 골격: 수명 객체(할당·로그·assert hook, `VerifyJoltVersionID`, `Factory`·`RegisterTypes`), `TaskJobSystem`, `PhysicsSettings`, `PhysicsWorldSubsystem`·`JoltPhysicsScene`(빈 장면), World 접점(2.2절), PhysicsSimulate 자리 연결(7.4절), Jolt `JPH_OBJECT_LAYER_BITS` 32(6.1절 — JL1 manifest·어댑터 경계 10.2절 갱신) | 7.4절 ER 개정 | 장면 생성·파괴·빈 step, job 풀 포화·종료 중 job(PC06), FP 상태, 할당 잔여 0, Editor·Preview World는 step 없음 |
| PH2 | (하위 단계 PH2a–PH2d — 7.1.2절) 어댑터 변환 함수(어댑터 경계 7절)와 PA 시험(7.1.2절 배정), Box·Sphere·Capsule·Cylinder body, 운동 종류, step, 결과 반영(3.4절 — 스텝별 활성·잠든 body 합), Kinematic 이동·teleport·목표 없는 스텝 정지(3.3절), 재생성 표(2.3.1절 — simulate 켤 때 떼기) | PH1, CM3 형상 컴포넌트(CR16 데이터) | 7.1.2절 PH2 배정 PA, 수명 시험, 결정성(같은 binary 상태 hash — PC03), 한 프레임 여러 스텝의 Kinematic 정지·프레임 안에서 잠든 body 반영 |
| PH3 | 질의(4장)와 필터 사상(6.1–6.3절 — CD3), `HitResult`·`OverlapResult` | PH2 | 4.4–4.5절 규칙 표 시험(채널·ObjectTypes·profile, 무시 목록, mobility, 시작 겹침, 정렬·자르기), PC02 |
| PH4 | `MoveComponent` sweep·침투 해소(5.1–5.2절), overlap(5.3절), hit(5.4절, sweep·시뮬레이션), probe(6.4절) | PH3, CR7(구독형 이벤트 — 없으면 가상 hook만) | UE 순서 시험(발행 순서·대칭 장부·재진입), pullback 수치 |
| PH5 | 힘·속도·질량·잠듦 API(2.6–2.7절), DOF·CCD·body 값 사상(6.5절), 기본 물리 재질·결합 방식(6.6절) | PH2 | PA04·PA05와 API 왕복, 잠듦 이벤트 |
| PH6 | weld(2.8절), 메시 단순 충돌·complex-as-simple(StaticMesh — D2b 뒤), 쿠킹 형상 너겟(계획 16.4절 cook — PC05) | PH2, StaticMesh 에셋(MA2b), 에셋 쿠킹 | weld 질량·질의 결과 컴포넌트, PC05 |
| (후속) | 캐릭터 이동(CM13), constraint 등(CM14), 지형 heightfield(CM18), ragdoll(CM7), 큰 월드 PhysicsRegion(계획 16.5절 — PC07), 플랫폼 결정성(PC04·PC08) | | |

### 7.1.1 PH1 구현 세부(2026-11-25)

사용자 결정(2026-11-25): A1–A6과 화면 마일스톤(SM1–SM6)이 끝나 계획 순서의 다음으로 PH1을 지금 시작한다. World 접점은 이 세션이, PhysicsSimulate 엔진 자리는 runtime 세션(rocks-f5)이 넣는다(7.4절).

**파일.**

| 위치 | 내용 |
|---|---|
| World `Public/World/Physics/PhysicsScene.h` | 추상 접점 `PhysicsScene`(Jolt 타입 없음). PH1 함수는 `Stats()`(body 수·활성 body 수·step 수 — `PhysicsSceneStats`) 하나이고, 2.2절의 나머지는 그 동작을 구현하는 단계(PH2–PH5)에서 더한다(PMD22) |
| World `World.h` | `GetPhysicsScene()`·`SetPhysicsScene(PhysicsScene*)`, 설정 문맥 읽기 `Settings()`(WorldDesc::settings — 물리 설정을 읽는다) |
| Physics `Public/Physics/PhysicsSettings.h` | `PhysicsSettings`(6.8절) 중 PH1이 쓰는 칸: `defaultGravityZ`, `maxBodies`·`maxBodyPairs`·`maxContactConstraints`, `tempAllocatorBytes`. 나머지 칸은 쓰는 단계에서 더한다 |
| Physics `Public/Physics/PhysicsWorldSubsystem.h` | `PhysicsWorldSubsystem`(2.1절), `Type()`, 진단용 `Scene()` |
| Physics `Private/JoltRuntime.*` | 프로세스 공유 자원(어댑터 경계 10.4절 순서)의 참조 수 획득·해제 |
| Physics `Private/TaskJobSystem.*` | Jolt job → Task(6.7절) |
| Physics `Private/JoltLayers.*` | object 층 bit 배치(6.1절)·broadphase 층 3개·PH1 필터 |
| Physics `Private/JoltPhysicsScene.*` | `PhysicsScene` 구현: Jolt `PhysicsSystem`·임시 할당기·job 시스템 소유, `Step(seconds)` |
| Physics `Private/PhysicsModule.cpp` | 수명 객체(`RegisterSettings`만 — Jolt 공유 자원은 장면이 쥔다, PMD21) |
| Physics `Tests/` | PH1 완료 기준 시험 |

**Jolt 공유 자원(PMD21).** 할당 hook(프로세스 수명 `SystemAllocator` — 글꼴 라이브러리와 같은 방식, 실패는 진단 후 fatal)·`Trace`(구조화 로그 `Physics.Jolt`)·`AssertFailed`(Fatal — Rocks assert와 같다) → `VerifyJoltVersionID` → `Factory` → `RegisterTypes`를 참조 수 0 → 1에서 하고, 1 → 0에서 `UnregisterTypes` → `Factory` 삭제를 한다. `PhysicsWorldSubsystem`이 장면마다 하나를 쥔다. 모듈 수명 객체는 쥐지 않는다 — 장면이 없을 때 Jolt 전역 상태를 비워 두어, 모듈을 시작한 프로세스에서도 Jolt를 직접 쓰는 연결 시험과 겹치지 않고, 모듈 로더 없이 World를 만드는 시험·도구도 같은 경로를 쓴다(버린 대안: 모듈 `Startup`에서 획득 — 2.1절 원안. 공유 형상 캐시가 생기는 PH6에서 수명을 다시 본다). hook 함수 포인터는 참조 수 0 → 1마다 설정하고 해제 때 되돌리지 않는다(Jolt가 남긴 블록을 다른 해제 함수가 받지 않게 — 연결 시험처럼 hook을 바꾸는 코드가 사이에 있어도 다음 획득이 다시 건다). 살아 있는 hook 블록 수를 세어 시험이 잔여 0을 확인한다. 획득·해제는 잠금으로 직렬화한다.

**job scheduler(PMD20).** 엔진이 World에 빌려주는 worker pool은 지금 `World::GcScheduler()`(WorldDesc::gcScheduler) 하나다. 물리는 그것을 빌려 쓴다(GC 표식은 Tick 끝, 물리 step은 Physics 단계라 같은 프레임 안에서 겹치지 않는다). 없으면 `GetMaxConcurrency` 1이고 `QueueJob`이 제출 thread에서 바로 실행한다(Jolt `JobSystemSingleThreaded`와 같은 동작). 엔진이 일반 Task pool을 World에 주게 되면 그쪽으로 바꾼다(계획 16.1절 "Game·Render·IO 예약분 제외"는 그때).

**job 수명.** Jolt `Job`은 미리 잡은 고정 목록(`FixedSizeFreeList<Job>`, `cMaxPhysicsJobs` 2048)에서 만든다. Jolt는 한 `PhysicsSystem`의 동시 job 수가 이 값을 넘지 않게 만든다(넘으면 Jolt assert — Fatal). `QueueJob`은 job 참조를 하나 더 쥐고(`AddRef`) Task에 `Execute` → `Release`를 제출한다. 제출이 실패하면(scheduler 포화 `CapacityExceeded`·할당 실패·종료 중) 그 자리에서 실행하지 않고 미룬 job 목록(미리 잡은 `cMaxPhysicsJobs`칸)에 둔다. Jolt job 함수는 자기 body 접근 권한 범위(`BodyAccess::Grant`) 안에서 다음 job의 의존을 풀어 `QueueJob`을 부르므로, 그 자리에서 실행하면 권한 범위가 겹쳐 Jolt assert가 난다(PH1 시험에서 확인 — 계획 16.1절의 "포화 시 제출 thread에서 즉시 실행"을 이렇게 고친다). 미룬 job은 Jolt barrier를 기다리는 thread가 실행하고(`WaitForJobs`는 barrier에 든 실행 가능한 job을 대기 thread에서 실행한다 — step의 job은 모두 barrier에 든다), worker도 자기 job을 끝낸 뒤 목록을 비운다. Jolt `JobSystemThreadPool`은 대기열이 차면 sleep으로 기다리는데 Rocks는 그렇게 하지 않는다(타이밍 의존 금지). `WaitForJobs`가 끝나도 Task 작업이 아직 `Release`를 하지 않았을 수 있으므로(Jolt `JobSystem.h` 설명) 남은 Task 작업 수를 센다. step마다 `Update` 뒤에 그 수가 0이 될 때까지 조건 변수로 기다리고(sleep 없음) 미룬 목록의 참조를 놓는다 — 그러지 않으면 늦게 끝난 Task와 미룬 목록이 쥔 job이 다음 step으로 넘어가 고정 목록이 찬다(PH1 시험에서 간헐적으로 확인). job 시스템 파괴(장면 파괴)도 같은 대기를 한다. Task body·미룬 목록의 저장소는 프로세스 수명 할당기다 — `Function` 정리는 capture 소멸(남은 수를 줄여 장면 파괴를 풀어 준다) 뒤에 저장소를 해제하므로, job 시스템 멤버 할당기면 장면이 사라진 뒤 worker가 해제할 수 있다(리뷰 반영). 동시 실행 수(`GetMaxConcurrency`)는 생성 때 고정한다(Jolt가 한 `Update` 안에서 여러 번 읽는다).

**FP 환경.** Jolt는 step의 job을 실행하는 모든 thread의 FP 상태가 같아야 한다(어댑터 경계 10.4절). worker의 job과 장면 thread의 `Update`를 모두 `platform::RunWithDeterministicFloatingPoint`(nearest-even, FTZ·DAZ 끔, trap 끔 — CoreDataFoundations 2.1절) 안에서 실행해 구조적으로 맞춘다. 환경을 바꿔야 했던 횟수를 진단으로 센다. 시험은 장면 thread를 FTZ 환경(`RunWithRealtimeAudioFloatingPoint`)에 두고 step해도 상태 byte가 같고 교정 횟수가 잡히는지 본다.

**층과 PH1 필터.** object 층은 6.1절의 32 bit 배치를 그대로 쓴다(반입 정의 `JPH_OBJECT_LAYER_BITS` 32 — 어댑터 경계 10.2절). broadphase 층 3개(Static·Moving·QueryOnly)는 운동 묶음 bit로 정한다. 시뮬레이션 쌍 필터는 6.2절 1단계(둘 다 Static, QueryOnly, physics·probe 표지 없음 → 거짓)까지만 하고 그 밖은 참이다. 응답 묶음 판정(2단계 이후)은 응답 묶음 표가 생기는 PH3에서 더한다.

**World 접점 규칙.** `SetPhysicsScene`은 World가 `Initialized`(Subsystem 초기화가 이 상태에서 돈다 — `Initialize`가 끝난 뒤 `BeginPlay` 전도 이 상태다) 또는 `TearingDown`(정리)일 때만 받고, 다른 상태는 `InvalidArgument`다. 같은 World에 장면이 이미 있는데 null 아닌 다른 장면을 넣으면 `AlreadyExists`다. body가 있는 동안 바꾸지 못하게 하는 검사는 body가 생기는 PH2에서 더한다.

**step.** `JoltPhysicsScene::Step(seconds)`는 `PhysicsSystem::Update(seconds, collisionSteps 1, 임시 할당기, job 시스템)`이다. 중력은 `defaultGravityZ`(cm/s²)를 m/s²로 바꿔 Jolt +Y 축에 둔다(축 A — Rocks Z → Jolt Y, 어댑터 경계 2절. 변환 함수 묶음은 PH2). `PhysicsWorldSubsystem`은 Game·Tryout World에서만 엔진 자리 `EngineStage::PhysicsSimulate`(엔진 런타임 15.4절 — rocks-f5 a438860b)에 `SimulateStep(문맥 delta = 고정 스텝 길이)`을 걸고 정리 때 뗀다(3.1절, PMD16 — World는 종류로 거르지 않는다). 자리가 이미 쓰였으면 초기화가 `AlreadyExists`로 실패한다. 호스트 Sandbox가 GameInstance의 World Subsystem 목록에 넣고 Physics 설정 타입을 설정 문맥에 등록한다(1.2절 — Sandbox 화면 자체 시험의 Game World도 장면을 만든다, 설정 문맥이 없으면 기본 설정). 확인은 Windows(EngineTests·Sandbox)뿐이다 — Android·Mac Sandbox 링크에 Physics·Jolt가 처음 들어가므로 그 빌드는 CI·다음 Android 확인 때 본다(미확인).

### 7.1.2 PH2 하위 단계와 PA 시험 배정(2026-11-26)

PH2는 크기가 커서 넷으로 나눈다(사용자 지시 "순서대로" — PH1 바로 뒤). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH2a | 어댑터 변환 함수(어댑터 경계 3·7절)와 그것만으로 고정되는 수용 시험 | PH1 |
| PH2b | `PhysicsScene` body 접점(`CreateBody`·`DestroyBody`·`SetBodyTransform`·`ReadKinematics` — 2.2절), 운동 종류(2.5절), Box·Sphere·Capsule 형상(어댑터 경계 4절 scale 규칙), `PrimitiveComponent` 물리 시스템 상태(2.3절). Cylinder body는 `CylinderComponent`(CM3)가 생기는 때(CY2) | PH2a, World 코드는 이 세션이 넣는다(2026-11-26 runtime 세션 동의) |
| PH2c | step 결과 반영(3.4절), Kinematic 이동·teleport·목표 없는 스텝 정지(3.3절), 결정성(PC03) | PH2b |
| PH2d | 재생성 표(2.3.1절 — simulate 켤 때 떼기) | PH2c, 엔진 런타임 `RecreateSystemStateNow`(7.4절) |

**PH2a 변환 함수.** Physics private `JoltConversions.h`(inline). 물리량마다 이름 있는 함수 하나(어댑터 경계 7절): 위치(지역 원점 o를 double에서 먼저 뺀 뒤 m로 — 6절)·변위·box 반폭·길이·선속도·선가속도·각속도·힘·선형 충격량·토크·각충격량·회전·법선·관성(주축 값 + 주축 회전 → Jolt `Mat44` = R'·D'·R'ᵀ, 곧 A·I·Aᵀ)·밀도. 축 사상 A(j = (r.y, r.z, r.x))는 한 곳의 두 함수(`AxisToJolt`·`AxisFromJolt`)만 쓰고, 크기 변환은 Core `UnitConversion.h` 함수만 쓴다. 크기 변환은 double에서 한 뒤 float로 한 번 반올림한다(Jolt float build).

**PA 시험 배정.** 어댑터 경계 9절의 PA01–PA13은 필요한 기능이 들어오는 단계에서 고정한다(PC09는 모두 통과한 시점이다).

| 시험 | 단계 | 이유 |
|---|---|---|
| PA01 방향 상수, PA02 각속도·quaternion, PA03 중력, PA04 힘·충격량, PA05 토크·관성, PA10 캡슐 축, PA13 테이퍼드 캡슐 | PH2a | 변환 함수와 Jolt body·형상을 시험이 직접 만들어 고정한다. 형상 변환 코드(컴포넌트·에셋 → Jolt 형상)는 고정하지 못하므로 PA10은 PH2b에서 `CapsuleComponent` 경로로, PA13은 PH6에서 단순 충돌 `CollisionTaperedCapsule` 경로로 다시 돌린다 |
| PA09 큰 월드 | PH2a(PA03 부분)·PH6(PA06 부분) | 지역 원점 변환은 PH2a, 메시는 PH6 |
| PA08 접촉 법선 부호 | PH3(sweep)·PH4(시뮬레이션 접촉) | 질의 결과 변환은 PH3, 접촉 hit 변환은 PH4(5.4절) |
| PA06 메시 앞면, PA11 거울 인스턴스 | PH6 | 렌더 메시 쿠킹 교환(어댑터 경계 4절) |
| PA07 heightfield | 지형(CM18) | heightfield 쿠킹 |
| PA12 좌우 바퀴·조향 | 차량 단계(개별 모듈) | 차량 constraint |

### 7.1.3 PH2b 구현 세부(2026-11-26)

runtime 세션(World 담당) 동의로 World 쪽 코드도 이 세션이 넣는다. Actor.h는 건드리지 않는다.

**World 쪽 타입**(`World/Physics/`, Jolt 없음 — 파일마다 타입 하나):

| 파일 | 내용 |
|---|---|
| `PhysicsBodyHandle.h` | `PhysicsBodyHandle { uint32 index; uint32 generation; }`(0 세대 = 없음 — 계획 16.1절) |
| `BodyGeometryKind.h`·`BodyGeometry.h` | `BodyGeometryKind { None, Box, Sphere, Capsule }`와 `BodyGeometry`(scale 전 값 cm: box 반폭, 구 반경, 캡슐 반경·반높이 **반구 포함**(UE `CapsuleHalfHeight`)) |
| `BodyMotion.h` | `BodyMotion { Static, Kinematic, Dynamic }`(2.5절) |
| `TeleportMode.h` | `TeleportMode { None, TeleportPhysics, ResetPhysics }`(1.3절) |
| `BodyCreateDesc.h` | 형상, 월드 `Transform`(scale 포함), 운동 종류, 실효 충돌(`ResolvedCollision`), `const BodySettings*`, 깨어서 시작할지 |
| `BodyKinematics.h` | 위치(body 원점 cm)·회전·선속도(cm/s)·각속도(rad/s)·깨어 있음 |

`PhysicsScene`에 `CreateBody(const BodyCreateDesc&) → Result<PhysicsBodyHandle>`, `DestroyBody(handle)`, `SetBodyTransform(handle, const Transform&, TeleportMode)`, `ReadKinematics(handle) → Result<BodyKinematics>`를 더한다(2.2절의 이름 그대로). 없는 handle·옛 세대는 `StaleHandle`(Destroy는 무시)이다.

**형상**: `PrimitiveComponent`의 가상 `BuildBodyGeometry(BodyGeometry&) const`(기본 거짓 — 형상 없음)를 Box·Sphere·Capsule 컴포넌트가 채운다(2.4절의 `BuildCollisionGeometry`를 PH2b는 형상 하나로 줄인 것 — compound는 PH6 메시 단순 충돌 때 넓힌다). Physics가 scale을 UE 규칙으로 형상 크기에 굽는다(어댑터 경계 4절): box 반폭 × |scale| 축별·구 반경 × min |scale|(둘 다 최소 `KindaSmallNumber`), 캡슐은 반높이 = 반높이 × |Z|(최소 0.1 cm)·반경 = 반경 × max(|X|, |Y|)를 [0.1, 반높이]로 clamp·원통 길이 = 2·(반높이 − 반경)(최소 0.1 cm — UE `FKSphylElem`과 같다). 0인 scale 축은 `KindaSmallNumber`로 바꾼다(2.3절). 크기 0은 거부하지 않는다(UE도 body를 만든다). body transform은 행렬 분해 없이 월드 위치·회전·scale을 따로 쓴다(0 scale·shear·반사에서도 값이 있다 — UE `GetComponentTransform`). 컴포넌트 캡슐 반높이는 반경 이상으로 올린다. 형상 공유 캐시는 PH6에서(지금은 body마다 만든다).

**body 값**(PH2b 몫 — 나머지 6.5절 표는 PH5): 운동 종류, object 층(채널 = object type, 운동 묶음 = Static·Moving, 실효 충돌 켬이 QueryOnly면 QueryOnly 묶음, 참여 표지 physics·query·probe는 실효 충돌 켬에서, 응답 묶음 색인 0 — 표는 PH3), `enableGravity`(중력 배율 1·0), `linearDamping`·`angularDamping`(값 그대로 — Jolt 기본 0.05에 맡기지 않는다), 질량은 형상 밀도(UE 기본 물리 재질 밀도 1 g/cm³ — Jolt 기본 1000 kg/m³와 같다). 질량 덮어쓰기·배율·질량 중심·관성 배율은 PH5.

**컴포넌트 수명**(2.3절): `PrimitiveComponent::WantsSystemState(Physics)` = World에 장면이 있음 && 형상이 있음 && 실효 충돌 켬 ≠ `NoCollision`(2.3절의 `alwaysCreatePhysicsState`는 `BodySettings`에 아직 없다 — 그 필드가 생기는 PH5). `CreateSystemState(Physics)`가 실효 충돌을 해석하고(`TryResolveCollision`) `BodyCreateDesc`를 채워 `CreateBody`, handle은 NATIVE 필드다. 시작 잠듦: `startAwake`가 거짓이고 소유 Actor가 시작(OnStart) 전이면 잠든 채 넣는다. `DestroySystemState(Physics)`가 `DestroyBody`. Static mobility에 `simulatePhysics`면 Warning `SimulateOnStaticBody`(2.5절)와 Static.

**setter**: `SetSimulatePhysics(bool)`(UE 이름 — 2.6절)를 PH2b에 미리 둔다. 지금은 값만 바꾸고, body가 있을 때의 즉시 전환은 PH2d다. `startAwake` 등 다른 body 값 setter는 PH5다(시작 잠듦 시험도 PH5로 옮긴다).

**transform**: `PrimitiveComponent::OnTransformUpdated`가 body가 있으면 `SetBodyTransform(월드 transform, TeleportMode::None)`을 부른다(3.3절). PH2b의 `SetBodyTransform`은 모든 운동 종류를 즉시 옮기고(`SetPositionAndRotation`, Dynamic 속도 유지) 잠든 Dynamic을 깨운다(UE `SetGlobalPose`). Kinematic의 `MoveKinematic` 경로·결과 반영의 skip 표지는 PH2c, scale 변화의 형상 갱신은 PH2d(재생성 표)다.

**시험**(World·컴포넌트 공개 API만 — Modular에서도 돈다): 등록·파괴의 body 수, NoCollision·장면 없는 World는 body 없음, mobility·simulate → 운동 종류(Static에 simulate면 경고 뒤 Static), 떨어지는 구의 `ReadKinematics`, PA10을 `CapsuleComponent` 경로로(반구 포함 반높이 의미), box scale을 바닥 위 구의 정지 높이로 확인, 컴포넌트 이동이 Kinematic body를 옮김, 옮긴 잠든 Dynamic이 깨어남, 크기·scale 0 형상과 비균일 scale 부모 아래 회전한 자식도 body가 생김. 한계: World 물리 장면 교체(`SetPhysicsScene` null → 새 장면) 뒤 남은 컴포넌트 handle은 새 장면의 같은 칸을 가리킬 수 있다 — 교체는 지금 Subsystem 수명 안에서만 일어나 남은 컴포넌트가 없다. 등록된 body가 있으면 교체를 거부하는 검사는 PH2d(재생성)에서 더한다. 등록 실패 되돌림 시험(용량 초과)은 PH5(설정 문맥 시험 기반)에서.

## 7.2 시험 원칙

- 각 단계 시험은 UE 원본 동작을 기대값으로 둔다(행 인용). 수치 적분은 step 크기에서 유도한 허용 오차, 방향·단위 사상은 정확 일치(어댑터 경계 9절).
- 물리 없는 World(접점 null)에서 컴포넌트가 body 없이 돌고 질의가 hit 없음을 돌려주는 경로도 시험한다.
- step 중 접점 호출 거부(debug Fatal)는 프로세스 시험(죽는 시험 — 기존 crash probe 방식)으로 확인한다.

## 7.3 미확인

없다. PMU1·PMU5·PMU6은 개정 1·2, PMU2는 개정 4(Jolt 수정과 시험 — 어댑터 경계 10.6절), PMU3·PMU4는 개정 5(PMD18·PMD19)에서 닫았다.

## 7.4 다른 문서 개정 요청

| 문서 | 요청 | 맡는 곳 |
|---|---|---|
| 엔진 런타임 8·15절 | Physics 단계 **고정 스텝마다** 사용자 Fixed hook 뒤에 엔진 자리 `PhysicsSimulate`를 하나 더한다. 등록은 `SetEngineStage`와 같은 방식 | runtime 세션(rocks-f5) — 반영됨(a438860b, 2026-11-25) |
| 엔진 런타임 23절 | 즉시 다시 만들기 API `RecreateSystemStateNow(kind)`(2.3.1절 — ERD25의 PreRender 일괄과 별도, 실패 정책은 같음) | runtime 세션 — PH2 전 |
| 엔진 런타임 23·24절 | `PrimitiveComponent`의 물리 시스템 상태 판정·생성(2.3절), `OnTransformUpdated` hook에서 body 이동(3.3절), 이동 API의 `TeleportMode` 인자, simulate 켤 때 떼기(2.3.1절), `MoveComponent`·`UpdateOverlaps`(5장), `World::GetPhysicsScene`·`SetPhysicsScene`(2.2절) | World 코드 — PH2·PH4 때(누가 구현할지는 그때 정한다) |
| 컴포넌트 도입 CR16 | 상태를 "설계 있음(PhysicsModule)"으로, CM3에 `CylinderComponent` 데이터 | 반영함(2026-11-25, PH0) |
| 전체 구현 계획 16절 | 16.1절 `PhysicsWorld` 인터페이스 스케치를 이 문서 2.2절 접점으로 대체, 16.1–16.3절의 command 구조(`Enqueue`, "callback 중 body 추가·삭제는 다음 command batch로", "쓰기는 Physics lane의 command 단계")를 동기 step 결정(3.2절 PMD6)으로 고치고 비동기 step 때로 미룸 | 반영함(2026-11-25, PH0) |

## 7.5 결정

| ID | 결정 | 근거·버린 대안 |
|---|---|---|
| PMD1 | 모듈 `Physics`(World·Collision 위), Jolt는 private | World는 Physics를 몰라야 한다(물리 없는 World 지원) |
| PMD2 | World 모듈에 추상 접점 `PhysicsScene`, World가 포인터를 갖는다 | UE `UWorld::GetPhysicsScene()`과 같은 자리. 버린 대안: WorldIntegration처럼 위 모듈이 컴포넌트를 읽는 방식 — 컴포넌트가 이동 중 sweep·body 생성을 직접 요청해야 해서 맞지 않는다 |
| PMD3 | 장면은 `PhysicsWorldSubsystem`이 만든다, 호스트가 `GameInstanceConfig::worldSubsystems`로 넣는다 | Gameplay가 Physics에 의존하면 Gameplay를 쓰는 모든 도구·시험이 Jolt를 링크한다. 버린 대안: GameInstance가 `GameplayWorldSubsystem`처럼 직접 넣기 |
| PMD4 | body는 시스템 상태(Physics)로 수명, 충돌 바뀜은 즉시 재생성 | UE setter 안 재생성(`EnsurePhysicsStateCreated`) |
| PMD5 | 고정 스텝마다 `PhysicsSimulate`, 결과 반영·이벤트는 프레임에 한 번 | 엔진 런타임 고정 스텝 구조. UE 기본 가변 dt와 다르다(3.5절) |
| PMD6 | 동기 step, 명령 큐 없음, step 밖 즉시 쓰기 | 같은 프레임 질의가 바뀐 값을 본다(UE 동작). 비동기 step은 후속 |
| PMD7 | Kinematic은 즉시 옮기고(질의가 본다) 시작 자세를 기록해 첫 스텝에서 `MoveKinematic`, 목표 없는 스텝은 속도 0 | UE는 GT 위치를 바로 바꾸고 프레임당 step 한 번이다. Jolt `MoveKinematic` 속도가 남는 문제(`Body.h:221`)를 막는다 |
| PMD8 | 질의 함수는 필터 종류를 `QueryFilter` 인자로 합친다 | UE 27개 → 9개. 의미는 같다 |
| PMD9 | `traceComplex` 기본은 거짓(함수 기본 인자도) | UE `DefaultQueryParam`(참)과 struct 기본(거짓)이 다른 함정을 없앤다 |
| PMD10 | 같은 time hit는 body handle 순 | Jolt 결과 순서에 기대지 않는 결정성 |
| PMD11 | overlap은 질의 기반, 시뮬레이션 쌍 필터는 Block만 | UE와 같다 |
| PMD12 | object 층 32 bit = 채널 6 + 운동 묶음 2 + 참여 표지 3 + 응답 묶음 색인 21 | broadphase 층은 object 층으로만 정해진다(`BroadPhaseLayer.h:70`). 개정 1의 16 bit(채널 + 색인)는 Static·Dynamic, 질의 전용을 구분하지 못했다 |
| PMD13 | probe = 접촉마다 `mIsSensor` | Jolt가 접촉 단위 sensor를 지원 |
| PMD14 | `normalImpulse`는 solver 충격량의 법선 방향 벡터 합(Jolt 접촉 캐시를 읽는 추적 가능한 수정), CCD로만 닿은 쌍은 `EstimateCollisionResponse` 추정 | UE hit의 `NormalImpulse`가 solver 충격량이다. Jolt에 공개 API가 없다(버린 대안: 늘 추정 — 충돌 세기가 UE와 달라진다. 운동량 변화로 역산 — 여러 쌍이 겹치면 나눌 수 없다) |
| PMD15 | Dynamic body에 대한 `SetBodyTransform`은 명시 호출만 받고 부착 전파는 무시 | Fixed hook 안의 낡은 transform이 body를 되돌리지 않게(3.2절) |
| PMD16 | step은 Game·Tryout World만, Editor·Preview는 질의 전용 장면 + teleport 이동 | UE 편집 World는 물리 tick을 돌리지 않는다(`BodyInstance.cpp:2796–2802` `bEditorWorld`) |
| PMD17 | 결합 방식은 프로세스 전역 | Jolt 결합 함수는 상태 없는 함수 포인터(`ContactConstraintManager.h:53`), UE도 프로젝트 설정 하나 |
| PMD18 | `maxLinearVelocity` 기본은 사실상 없음(√FLT_MAX cm/s) | UE body 상한이 같다(6.8절). Jolt `mMaxLinearVelocity`(기본 500 m/s — `Physics/Body/BodyCreationSettings.h:111`)는 속도 clamp에만 쓰이고(`Physics/Body/MotionProperties.inl:23–31`, soft body 제외) `BodyInterface`의 속도 설정 함수도 clamp판을 부른다(`Physics/Body/BodyInterface.cpp:595–603`). 버린 대안: Jolt 기본 500 m/s — 빠른 탄환 body가 UE와 달리 느려진다. 큰 속도의 안정성은 CCD(`continuousCollision`)로 다룬다 |
| PMD20 | job은 World가 빌려주는 worker pool(`GcScheduler`), 없으면 제출 thread 실행 | 엔진이 World에 주는 pool이 지금 그것뿐이다. 물리 전용 pool은 계획 16.1절이 금지한다(7.1.1절) |
| PMD21 | Jolt 공유 자원은 장면마다 참조 수로 쥔다(모듈 수명 객체는 쥐지 않는다) | 장면이 없으면 Jolt 전역 상태가 비어 있고, 모듈 로더 없이 World를 만드는 시험·도구도 같은 경로를 쓴다(7.1.1절) |
| PMD22 | `PhysicsScene` 접점은 단계마다 그 동작과 함께 넓힌다(PH1은 `Stats`만) | 구현 없는 순수 가상을 미리 두지 않는다(2.2절 목록이 목표) |
| PMD19 | DOF 잠금은 `mAllowedDOFs`(월드 축, 속도 성분 제거), CustomPlane만 `SixDOFConstraint` | UE도 월드 축·생성 시점 기준이다(6.5절). 한계 둘: (1) UE는 joint라 잠긴 축으로 teleport하면 생성 시점 값으로 끌려 돌아가지만(joint frame을 갱신하지 않는다) Rocks는 teleport한 값을 유지한다 — UE의 부작용으로 보고 따르지 않는다. (2) UE `SixDOF`의 회전 잠금은 twist·swing 분해 각을 잠그고 Jolt는 월드 축 각속도 성분을 없앤다. 회전 축 하나만 잠그고 나머지 둘로 크게 돌면 결과 자세가 다를 수 있다(두 축 이상 잠금·평면 방식은 같다) |

## 7.6 개정 이력

- 개정 8(2026-11-26): PH2a 완료(fcfe9ad4), PH2b 구현 세부(7.1.3절).
- 개정 7(2026-11-26): PH1 완료(PhysicsSimulate 연결·Sandbox 등록), PH2 하위 단계와 PA 시험 배정(7.1.2절).
- 개정 6(2026-11-25): PH1 착수 — 구현 세부(7.1.1절), PMD20–PMD22.
- 개정 5(2026-11-25): PMU3·PMU4를 UE 원본으로 닫았다. `maxLinearVelocity` 기본은 사실상 없음(PMD18), DOF 잠금 기준은 월드 축·생성 시점(PMD19, 한계 둘). 미확인 항목이 없다.
- 개정 4(2026-11-25): PMU2를 닫았다. Jolt 접촉 캐시의 solver 충격량을 읽는 추적 가능한 수정과 시험(어댑터 경계 10.6절)으로 `normalImpulse`를 UE처럼 solver 값으로 바꿨다(6.4절, PMD14).
- 개정 3(2026-11-25): UE용 Jolt 공개 플러그인 UnrealJolt(369ef176)와 대조했다(어댑터 경계 11절). 설계는 바뀌지 않았다: 좌표(행렬식 +1)·채널 응답 필터·`MoveKinematic`·결과 반영에 보간 없음은 그대로 두고, PMU2는 UnrealJolt도 추정값을 써서 닫지 않는다.
- 개정 2(2026-11-25): 리뷰어 1명 대조 반영(치명 2 — object 층 bit가 broadphase 층을 정하지 못함, Kinematic 속도 남음 / 중요 8 — Kinematic 즉시 이동, simulate 컴포넌트 떼기, 프레임 안에서 잠든 body 반영, Fixed hook 낡은 transform, 즉시 재생성 API·실패 정책, Jolt 마찰은 body 값·결합 함수 전역·반발 문턱, startAwake·overlap은 Actor 단위, Editor·Preview step 없음 / 경미 8 — 계획 16.2·16.3절 개정 요청, `mAllowDynamicOrKinematic` 불필요, DOF 전부 잠금·CustomPlane, `updateMassWhenScaleChanges`, 접점에 침투·깨움·속도, 0 스텝 프레임 이벤트, 접촉 합치는 순서·hit 문턱, 인용 3곳, scale 0·traceComplex UE 차이, Gameplay 의존 대신 설정 목록). PMU6을 닫았다(시뮬레이션 컴포넌트는 부착되지 않으므로 순서 문제가 없다).
- 개정 1(2026-11-25): 초안. UE 5.8.4 조사 두 갈래(body 수명·step, 질의·이동·이벤트)를 근거로 썼다. 쓰는 중 UE 원본으로 확인해 PMU1(AllObjects에 이름 없는 채널 포함)·PMU5(기본 물리 재질·결합 방식)와 기본 DOF(Full3D)를 닫았다.
