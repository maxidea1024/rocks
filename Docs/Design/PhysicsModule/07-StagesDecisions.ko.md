# 물리 모듈 — 7. 단계·시험·결정

[목차](README.ko.md)

## 7.1 단계

모두 M4 물리 단계에서 계획 순서대로 한다(사용자 결정 2026-11-25 — 스펙만 지금). 같은 날 화면 마일스톤 뒤라 PH1부터 착수했다(7.1.1절). 묶음마다 설계 갱신 → 구현 → 리뷰어 1명 → 커밋.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| PH0 | 이 문서 | JL1·CD1·CD2 | 리뷰어 1명 반영 |
| PH1 | `Physics` 모듈 골격: 수명 객체(할당·로그·assert hook, `VerifyJoltVersionID`, `Factory`·`RegisterTypes`), `TaskJobSystem`, `PhysicsSettings`, `PhysicsWorldSubsystem`·`JoltPhysicsScene`(빈 장면), World 접점(2.2절), PhysicsSimulate 자리 연결(7.4절), Jolt `JPH_OBJECT_LAYER_BITS` 32(6.1절 — JL1 manifest·어댑터 경계 10.2절 갱신) | 7.4절 ER 개정 | 장면 생성·파괴·빈 step, job 풀 포화·종료 중 job(PC06), FP 상태, 할당 잔여 0, Editor·Preview World는 step 없음 |
| PH2 | (하위 단계 PH2a–PH2d — 7.1.2절) 어댑터 변환 함수(어댑터 경계 7절)와 PA 시험(7.1.2절 배정), Box·Sphere·Capsule·Cylinder body, 운동 종류, step, 결과 반영(3.4절 — 스텝별 활성·잠든 body 합), Kinematic 이동·teleport·목표 없는 스텝 정지(3.3절), 재생성 표(2.3.1절 — simulate 켤 때 떼기) | PH1, CM3 형상 컴포넌트(CR16 데이터) | 7.1.2절 PH2 배정 PA, 수명 시험, 결정성(같은 binary 상태 hash — PC03), 한 프레임 여러 스텝의 Kinematic 정지·프레임 안에서 잠든 body 반영 |
| PH3 | (하위 단계 PH3a–PH3c — 7.1.6절) 질의(4장)와 필터 사상(6.1–6.3절 — CD3), `HitResult`·`OverlapResult` | PH2 | 4.4–4.5절 규칙 표 시험(채널·ObjectTypes·profile, 무시 목록, mobility, 시작 겹침, 정렬·자르기), PC02 |
| PH4 | `MoveComponent` sweep·침투 해소(5.1–5.2절), overlap(5.3절), hit(5.4절, sweep·시뮬레이션), probe(6.4절) | PH3, CR7(구독형 이벤트 — 없으면 가상 hook만) | UE 순서 시험(발행 순서·대칭 장부·재진입), pullback 수치 |
| PH5 | 힘·속도·질량·잠듦 API(2.6–2.7절), DOF·CCD·body 값 사상(6.5절), 기본 물리 재질·결합 방식(6.6절) | PH2 | PA04·PA05와 API 왕복, 잠듦 이벤트 |
| PH6 | weld(2.8절), 메시 단순 충돌·complex-as-simple(StaticMesh — D2b 뒤), 쿠킹 형상 너겟(계획 16.4절 cook — PC05) | PH2, StaticMesh 에셋(MA2b), 에셋 쿠킹 | weld 질량·질의 결과 컴포넌트, PC05 |
| (후속) | 캐릭터 이동(CM13), constraint 등(CM14), 지형 heightfield(CM18), ragdoll(CM7), 큰 월드 PhysicsRegion(계획 16.5절 — PC07), 플랫폼 결정성(PC04·PC08) | | |

PH1–PH3의 구현 세부(7.1.1–7.1.9절)는 [7.1 단계별 구현 세부(PH1–PH3)](07a-StageDetailsPH1-PH3.ko.md)에 있다.

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
| 엔진 런타임 23절 | 즉시 다시 만들기 API `RecreateSystemStateNow(kind)`(2.3.1절 — ERD25의 PreRender 일괄과 별도, 실패 정책은 같음) | runtime 세션 — 반영됨(63cb977e, 2026-11-26) |
| 엔진 런타임 23·24절 | `PrimitiveComponent`의 물리 시스템 상태 판정·생성(2.3절), `OnTransformUpdated` hook에서 body 이동(3.3절), 이동 API의 `TeleportMode` 인자, simulate 켤 때 떼기(2.3.1절), `MoveComponent`·`UpdateOverlaps`(5장), `World::GetPhysicsScene`·`SetPhysicsScene`(2.2절) | World 코드 — PH2 몫은 이 세션이 넣었다(7.1.3·7.1.5절), `MoveComponent`·`UpdateOverlaps`는 PH4 |
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

- 개정 14(2026-11-27): PH3c1 완료(70cc3bcb), PH3c2 구현 세부(7.1.9절).
- 개정 13(2026-11-26): PH3b 완료(eff5569c), PH3c 하위 단계와 PH3c1 구현 세부(7.1.8절).
- 개정 12(2026-11-26): PH3a 완료(79b8779b), PH3b 구현 세부(7.1.7절).
- 개정 11(2026-11-26): PH2d 완료(05226c41 — PH2 끝), PH3 하위 단계와 PH3a 구현 세부(7.1.6절).
- 개정 10(2026-11-26): PH2c 완료(e67a082b), PH2d 구현 세부(7.1.5절 — 운동 종류 전환·필터·형상·scale·Mobility, 리뷰 반영).
- 개정 9(2026-11-26): PH2b 완료(6ba9c214 — 리뷰 반영: 크기 0·shear에서도 body, 옮긴 잠든 body 깨움), PH2c 구현 세부(7.1.4절).
- 개정 8(2026-11-26): PH2a 완료(fcfe9ad4), PH2b 구현 세부(7.1.3절).
- 개정 7(2026-11-26): PH1 완료(PhysicsSimulate 연결·Sandbox 등록), PH2 하위 단계와 PA 시험 배정(7.1.2절).
- 개정 6(2026-11-25): PH1 착수 — 구현 세부(7.1.1절), PMD20–PMD22.
- 개정 5(2026-11-25): PMU3·PMU4를 UE 원본으로 닫았다. `maxLinearVelocity` 기본은 사실상 없음(PMD18), DOF 잠금 기준은 월드 축·생성 시점(PMD19, 한계 둘). 미확인 항목이 없다.
- 개정 4(2026-11-25): PMU2를 닫았다. Jolt 접촉 캐시의 solver 충격량을 읽는 추적 가능한 수정과 시험(어댑터 경계 10.6절)으로 `normalImpulse`를 UE처럼 solver 값으로 바꿨다(6.4절, PMD14).
- 개정 3(2026-11-25): UE용 Jolt 공개 플러그인 UnrealJolt(369ef176)와 대조했다(어댑터 경계 11절). 설계는 바뀌지 않았다: 좌표(행렬식 +1)·채널 응답 필터·`MoveKinematic`·결과 반영에 보간 없음은 그대로 두고, PMU2는 UnrealJolt도 추정값을 써서 닫지 않는다.
- 개정 2(2026-11-25): 리뷰어 1명 대조 반영(치명 2 — object 층 bit가 broadphase 층을 정하지 못함, Kinematic 속도 남음 / 중요 8 — Kinematic 즉시 이동, simulate 컴포넌트 떼기, 프레임 안에서 잠든 body 반영, Fixed hook 낡은 transform, 즉시 재생성 API·실패 정책, Jolt 마찰은 body 값·결합 함수 전역·반발 문턱, startAwake·overlap은 Actor 단위, Editor·Preview step 없음 / 경미 8 — 계획 16.2·16.3절 개정 요청, `mAllowDynamicOrKinematic` 불필요, DOF 전부 잠금·CustomPlane, `updateMassWhenScaleChanges`, 접점에 침투·깨움·속도, 0 스텝 프레임 이벤트, 접촉 합치는 순서·hit 문턱, 인용 3곳, scale 0·traceComplex UE 차이, Gameplay 의존 대신 설정 목록). PMU6을 닫았다(시뮬레이션 컴포넌트는 부착되지 않으므로 순서 문제가 없다).
- 개정 1(2026-11-25): 초안. UE 5.8.4 조사 두 갈래(body 수명·step, 질의·이동·이벤트)를 근거로 썼다. 쓰는 중 UE 원본으로 확인해 PMU1(AllObjects에 이름 없는 채널 포함)·PMU5(기본 물리 재질·결합 방식)와 기본 DOF(Full3D)를 닫았다.
