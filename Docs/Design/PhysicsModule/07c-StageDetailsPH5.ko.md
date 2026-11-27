# 물리 모듈 — 7.1 단계별 구현 세부(PH5)

[목차](README.ko.md) · [7. 단계·시험·결정](07-StagesDecisions.ko.md) · [PH4](07b-StageDetailsPH4.ko.md)

7장 단계 표(7.1절)의 PH5 하위 단계 구현 세부다. 절 번호는 7장 그대로다.

### 7.1.15 PH5 하위 단계와 PH5a 구현 세부(2026-11-27)

PH5는 넷으로 나눈다(크기 — "순서대로"). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH5a | body 값 사상의 나머지(6.5절 — 질량·관성·질량 중심, 감쇠 외 속도 상한, solver 반복, gyroscopic, smooth edge), 기본 물리 재질·결합 방식·반발 문턱(6.6절), 상자 margin, 물리 설정 칸(6.8절) | PH4 |
| PH5b | DOF(6.5절 — 축 잠금·평면·CustomPlane `SixDOFConstraint`·설정 기본 DOF·모두 잠금 경고) | PH5a |
| PH5c | 힘·충격량·속도·질량 API와 body 값 setter(2.6절 — PA04·PA05를 컴포넌트 경로로) | PH5a |
| PH5d | 잠듦·깨움 API·이벤트(2.7절 — `OnComponentWake`·`OnComponentSleep`, `generateWakeEvents`)와 `OnComponentPhysicsStateChanged` | PH5c |

**질량(UE `BodyUtils::ComputeMassProperties`·`ApplyMassPropertiesModifiers` — `E/Private/PhysicsEngine/BodyUtils.cpp:48–141`, 호출 `BodyInstance.cpp:3486–3493`).** body를 만들거나 형상을 바꿀 때(질량 다시 계산이 참일 때) 다음 순서로 Jolt 질량 속성을 만들어 `EOverrideMassProperties::MassAndInertiaProvided`로 넣는다(Jolt 형상 기본 계산을 그대로 쓰지 않는다).
1. 형상 밀도 = 물리 재질 밀도(기본 1 g/cm³ → 1000 kg/m³ — UE 하한 수소 0.09 kg/m³)로 형상(질량 중심 이동 전)의 질량·관성을 구한다(Jolt `Shape::GetMassProperties` — 균일 밀도).
2. `overrideMass`가 거짓이면 질량 = max(`massScale` × 원시 질량^`RaiseMassToPower`, 0.001 kg)이다(UE 재질 `RaiseMassToPower` 기본 0.75 — `PC/Private/PhysicalMaterial.cpp:48`, clamp [KindaSmallNumber, 1]). 참이면 max(`massInKgOverride`, 0.001 kg)다.
3. 관성 × (새 질량 / 원시 질량).
4. `inertiaTensorScale`이 (1, 1, 1)에서 성분마다 1e-3 넘게 다르면 Chaos `ScaleInertia(bScaleMass 거짓)`(`CH/Public/Chaos/Utilities.h:537–557`): 대각 D에 대해 s = (½(Dx+Dy+Dz) − D) × scale²로 Ixx = sy+sz, Iyy = sx+sz, Izz = sx+sy, 비대각은 두 축 scale을 곱한다. 축은 Rocks 축이다(Jolt 관성을 축 A로 Rocks 축에 옮겨 계산하고 되돌린다).
5. 질량 중심 이동 `centerOfMassOffset`(cm, body 공간) × 컴포넌트 scale(UE `MassModifierTransform` — scale만)을 형상에 `OffsetCenterOfMassShape`로 감싸 넣고, 관성 대각에 UE 방식 그대로 m·dx², m·dy², m·dz²를 더한다(UE `p.ComNudgeAffectsInertia` 기본 참 — 평행축의 축별 단순형, `BodyUtils.cpp:101–111`. 물리적인 평행축 정리와 다르지만 UE 결과를 따른다).
- 비유한 질량 칸(`massScale`·`massInKgOverride`·`inertiaTensorScale`·`centerOfMassOffset`)은 body를 만들지 않는다(`InvalidArgument` — Jolt 질량 assert·NaN 시뮬레이션). 0이 된 주축(관성 배율 0 등)은 가장 큰 주축값 × 1e-6으로 올린다: Chaos는 0 관성을 잠긴 축으로 다루지만 Jolt는 역수를 내 무한대가 된다(한계 — 그 축이 아주 쉽게 돈다).
- `updateMassWhenScaleChanges`가 참이면 형상이 바뀔 때(scale·크기) 같은 순서로 다시 계산해 `MotionProperties::SetMassProperties`로 넣는다(Jolt `SetShape(updateMass)`의 기본 계산은 쓰지 않는다). 거짓이면 이전 질량·관성을 둔다(7.1.5절).
- Static body에도 같은 값을 넣는다(Jolt는 쓰지 않는다 — 나중에 Movable로 다시 만들 때 같은 경로).

**물리 재질 기본값(6.6절).** 재질 에셋 전에는 모든 body가 UE `UPhysicalMaterial` 기본값이다: 마찰 0.7, 반발 0.3, 밀도 1 g/cm³, `RaiseMassToPower` 0.75(`PC/Private/PhysicalMaterial.cpp:45–49`). Jolt `mFriction`·`mRestitution`(body 값)과 형상 밀도에 넣는다. 이름 있는 상수 `DefaultPhysicalMaterial…`(Physics private)다. UE 정지 마찰(`StaticFriction` 0)은 Jolt에 따로 없다(마찰 하나 — 한계).

**결합 방식.** 물리 설정 `frictionCombineMode`·`restitutionCombineMode`(`CombineMode { Average, Min, Multiply, Max }` — UE `EFrictionCombineMode`, 기본 Average·Average)를 장면의 Jolt `SetCombineFriction`·`SetCombineRestitution`에 상태 없는 함수 넷 가운데 하나로 넣는다. 함수 포인터는 PhysicsSystem(장면)마다라 장면마다 다른 설정이 가능하다(6.6절의 "프로세스 전역"은 틀렸다 — 정정). Jolt 기본(마찰 기하평균·반발 최대)은 쓰지 않는다.

**반발 문턱.** 물리 설정 `restitutionThreshold`(cm/s², 기본 1000 — Chaos `DefaultRestitutionThreshold` `CH/Public/Chaos/PBDRigidsEvolutionGBF.h:70`, CVar `p.CollisionRestitutionThreshold`) × solver step dt가 속도 문턱이다(`ContactVelocityNormal < −Threshold·Dt`면 반발 — `CH/Private/Chaos/Collision/PBDCollisionContainerSolver.cpp:89·163`, 60 Hz면 약 16.7 cm/s). Step이 dt가 바뀔 때 Jolt `PhysicsSettings::mMinVelocityForRestitution`(m/s)에 넣는다. UE 설정 `BounceThresholdVelocity`(200 cm/s — `PC/Private/PhysicsSettingsCore.cpp:42`)는 UE 5.8 어디에서도 읽지 않는다(PhysX 시절 칸 — 처음 설계의 정정).

**상자 margin.** UE Chaos 상자는 margin(둥근 모서리 반경)이 min(2 × 가장 작은 반폭 × `CollisionMarginFraction`, `CollisionMarginMax`)이다(`E/Private/PhysicsEngine/Experimental/ChaosInterfaceUtils.cpp:156`, 설정 기본 0.05·10 cm — `CH/Public/Chaos/PBDRigidsEvolutionGBF.h:65–66`). 바깥 크기는 그대로이고 모서리만 둥글다 — Jolt `BoxShape` convex radius와 같은 뜻이다. Jolt 기본 5 cm 대신 이 값을 넣는다(물리 설정 `collisionMarginFraction`·`collisionMarginMax`). 가장 작은 반폭을 넘지 않는다(Chaos `TBox::ClampedMargin` — `CH/Public/Chaos/Box.h:155–158`, Jolt `BoxShape`의 조건과 같다). 구·캡슐은 margin이 없다(형상 자체가 둥글다). 질량은 margin과 무관하다(Jolt·Chaos 둘 다 상자 부피).

**속도 상한·solver·그 밖(6.5절 표).**
- `mMaxAngularVelocity`: `overrideMaxAngularVelocity`면 `maxAngularVelocity`, 아니면 물리 설정 `maxAngularVelocity`(기본 3600 도/s — UE `MaxAngularVelocity` `:43`), 도/s → rad/s.
- `mMaxLinearVelocity`: 물리 설정 `maxLinearVelocity`(기본 `UnlimitedLinearVelocity` = √FLT_MAX cm/s — PMD18), cm/s → m/s.
- `overrideSolverIterations`면 `mNumPositionStepsOverride` = `positionIterations`, `mNumVelocityStepsOverride` = `velocityIterations`, 아니면 0(장면 기본 — Jolt 10·2).
- `gyroscopicTorque` → `mApplyGyroscopicForce`, `smoothEdgeCollisions` → `mEnhancedInternalEdgeRemoval`.

**물리 설정 칸 추가**(6.8절): `maxAngularVelocity`(도/s)·`maxLinearVelocity`(cm/s)·`restitutionThreshold`(cm/s²)·`frictionCombineMode`·`restitutionCombineMode`·`collisionMarginFraction`·`collisionMarginMax`(cm). 검증: 유한·음수 아님(속도 상한은 0보다 큼), 결합 방식은 알려진 값. `defaultDegreesOfFreedom`은 PH5b다.

**시험**(Jolt body 값을 직접 읽으므로 Monolithic — `PhysicsBodyValueSceneTests.cpp`, World 경로는 `PhysicsBodyTests`):
- 질량: 반경 20 cm 구의 질량이 (원시 질량)^0.75, `massScale` 2배, override 50 kg, override 0 → 0.001 kg. 관성은 같은 비율(구 2/5·m·r²), `inertiaTensorScale` (2, 1, 1)이 Chaos 식, 질량 중심 이동이 body 질량 중심 위치와 관성 대각에 들어간다.
- 형상 크기·scale 변경에서 `updateMassWhenScaleChanges` 참은 UE 식으로 다시 계산, 거짓은 그대로(7.1.5절에서 미룬 시험).
- 재질: body 마찰 0.7·반발 0.3. 결합: 반발 Average(0.3)·Multiply(0.09)가 바닥에 떨어진 구의 튀는 속도로 보이고, 값이 다른 두 body에서 결합 함수가 방식마다 다르다(Jolt 기본과 구분). 반발 문턱: 5 cm 낙하(약 99 cm/s)는 기본 문턱(16.7 cm/s)에서 튀고 큰 문턱에서는 안 튄다. Jolt 문턱 값 = 문턱 × dt.
- 반폭이 다른 상자의 관성 배율(축 index 확인), Kinematic 형상 변경의 질량 다시 계산, 거짓이면 관성도 그대로, 질량 중심 이동의 scale 변경 다시 감싸기, 질량 중심을 옮긴 상자의 sweep 모서리 hit가 면 법선(질의가 wrapper를 벗긴다).
- 상자 convex radius = min(0.1 × 가장 작은 반폭, 10 cm)(작은 상자·큰 상자).
- 각속도 상한(설정·body 덮어쓰기), 선속도 상한(사실상 없음), solver 덮어쓰기, gyroscopic·smooth edge 표지.
- 설정 검증(음수·비유한·모르는 결합 방식).

**한계.** 정지 마찰은 Jolt에 없다. solver 반복 덮어쓰기는 Chaos와 Jolt의 반복 뜻이 달라 같은 수가 같은 결과를 내지 않는다(6.5절 표). UE 5.8 Chaos는 body 재질의 결합 방식을 늘 쓰고(`ChaosEngineInterface.cpp:244–246`) 프로젝트 설정 결합 방식은 사실상 쓰이지 않는다 — 기본 Average가 같아 기본 결과는 같다. 재질 에셋·body별 재질·결합 방식 덮어쓰기(UE `bOverrideFrictionCombineMode`)는 에셋 단계다. 질의 형상도 body 형상(margin 포함)을 그대로 쓴다(7.1.9절 한계 그대로 — UE 질의는 정확한 상자).

**PH5a 리뷰 반영(2026-11-27).** 치명 0, 중요 2, 경미 6. 반발 문턱을 UE 5.8이 실제로 쓰는 Chaos 가속도 문턱 × dt로 바꿨다(`bounceThresholdVelocity` 칸은 ID 은퇴 — UE에서 읽히지 않는다). 질량 중심을 옮긴 상자에서 질의의 반대면 법선 보정(7.1.8절 `FindGeomOpposingNormal`)이 `OffsetCenterOfMassShape` subtype 때문에 빠지던 것을 고쳤다(안쪽 형상을 본다). 비유한 질량 칸 거부·0 주축 하한·질량 중심 이동 계산 한 곳, solver 반복 뜻 차이·UE 결합 방식 한계 기록, 각속도 상한 상수 중복 제거(Collision `DefaultMaxAngularVelocityDegrees`), 6.5·6.8절 표 갱신, 시험 보강(결합 함수, 반폭이 다른 상자, 문턱 쌍, 형상 변경 세 경우, 옮긴 상자 sweep)과 시험 상수·include 정리.

**후속(PH5 밖).** Sparks 이벤트 노출(EV4 — rocks-ee 22686a3d1)이 들어와 PH4e 이벤트 필드에 `ScriptAssignable`을 붙일 수 있다. 인자 struct `HitResult`에 `ScriptType`이 필요하고, 이름은 LevelScript LD9의 물리 이벤트 이름(`TriggerEntered`·`CollisionEntered` 등)과 겹치지 않게 정한다. 물리 단계 뒤 연동 단계에서 한다.

### 7.1.16 PH5b 구현 세부(2026-11-28)

자유도(DOF) 잠금이다(6.5절 `dofMode`·`lock*` 행, PMD19). UE `FBodyInstance::CreateDOFLock`·`ResolveDOFMode`·`GetLockedAxis`(`E/Private/PhysicsEngine/BodyInstance.cpp:802–925`, 호출 `InitDynamicProperties` `:4727`)를 따른다.

**UE 동작.** body 대 월드 구속(`FConstraintInstance`, 상대 없음) 하나를 body 초기화 때 만든다. 구속 frame은 월드 축이고 월드 쪽 위치는 만들 때의 body 위치다(그래서 잠근 축의 값은 만들 때의 위치·자세다).
- `ResolveDOFMode`: `Default`는 물리 설정 `DefaultDegreesOfFreedom`(`Full3D` → SixDOF, `YZPlane`·`XZPlane`·`XYPlane` → 같은 평면)이다.
- 평면(`YZPlane` 법선 X, `XZPlane` 법선 Y, `XYPlane` 법선 Z, `CustomPlane` 법선 `customDofPlaneNormal`): 법선 방향 이동을 잠그고, 법선 둘레 회전(twist)만 남기고 평면 안 두 축 회전(swing)을 잠근다. 축 평면은 `lockTranslation`·`lockRotation` 표지와 상관없이 잠근다(UE 조건 `bLock… || DOFMode != CustomPlane`). `CustomPlane`만 두 표지를 따른다.
- `SixDOF`: 월드 X·Y·Z 이동과 X·Y·Z 축 둘레 회전을 `lock*Translation`·`lock*Rotation`마다 잠근다(하나도 없으면 구속 없음).
- `None`, 잠금 축이 0(평면 법선 0 — `CustomPlane`), Static body는 잠금이 없다.

**Rocks 구현.**
- 축 평면과 SixDOF는 Jolt `EAllowedDOFs`(월드 축 이동·회전 속도 마스크 — `Physics/Body/AllowedDOFs.h:14–19`)로 표현한다. 축 A로 재배치한다: Rocks X → Jolt Z, Y → X, Z → Y(회전 축도 같다 — 행렬식 +1). Jolt는 속도만 막으므로 잠근 축의 값은 그 축의 마지막 값에 머문다(PMD19).
- `CustomPlane`: 법선 성분이 모두 `KindaSmallNumber` 이하(UE `IsNearlyZero`)이거나 두 표지가 거짓이면 잠금이 없다. 정규화한 법선이 축과 나란하면(성분 하나의 절댓값이 1 ± 1e-6) 축 평면과 같은 bit로 하되 표지마다 따로다(`lockTranslation` → 그 축 이동, `lockRotation` → 다른 두 축 회전). 비스듬하면 Jolt `SixDOFConstraint`(body 대 `Body::sFixedToWorld`, 월드 공간, X축 = 법선, Y축 = 법선에 수직인 축)를 건다: `lockTranslation`이면 `TranslationX` 고정, `lockRotation`이면 `RotationY`·`RotationZ` 고정.
- **구속 기준.** 구속 위치는 body 원점이다(UE `CreateDOFLock`의 `Pos2` = body 위치 — 질량 중심 이동이 있어도 같은 점). 기준 자세는 붙이는 때다: 만들 때, Kinematic → Dynamic 전환, teleport(`SetBodyTransform`), 형상 변경(질량 중심이 옮겨질 수 있고 Jolt 구속은 body 변경을 모른다 — `Constraint.h:162–166`). teleport 뒤 기준을 새 자리로 옮기는 것은 축 bit 잠금과 같은 동작이다(PMD19 한계 1 — UE는 생성 때 frame에 끌려 돌아간다). 구속은 칸이 쥐고 body를 지울 때 먼저 뗀다.
- **모두 잠긴 조합**(SixDOF 여섯 표지 — Jolt `EAllowedDOFs::None`은 허용되지 않는다, `AllowedDOFs.h:12`): 허용 자유도는 `All`로 두고 여섯 축을 모두 고정한 구속으로 제자리에 둔다(UE도 6축 구속으로 멈춘다). 진단은 없다.
- **Dynamic에만 적용한다.** Jolt 허용 자유도는 Kinematic의 `MoveKinematic` 속도도 막으므로(UE 구속은 Kinematic에 효과가 없다), Kinematic·Static은 `All`이고 구속이 없다. `SetBodyMotion`이 Kinematic ↔ Dynamic으로 바꾸면 칸에 둔 질량 속성으로 `MotionProperties::SetMassProperties(자유도, 질량)`를 다시 넣고, 남은 속도를 `SetLinearVelocityClamped`·`SetAngularVelocityClamped`로 새 마스크에 거른 뒤(`SetMassProperties`는 속도를 거르지 않는다) 구속을 붙이거나 뗀다. 형상 변경의 질량 다시 계산도 칸의 자유도를 쓴다.
- **gyroscopic 힘**은 회전 축을 하나라도 bit로 잠그면 끈다(Jolt `ApplyGyroscopicForceInternal`이 각속도를 축 마스크로 거르지 않아 잠긴 축으로 샌다). UE Chaos는 gyroscopic을 구속 뒤에 풀어 잠금이 이긴다 — Rocks는 그 경우 gyroscopic 효과가 없다(한계).
- **QueryOnly**: UE는 `CollisionEnabled`가 물리 없음이어도 Dynamic이면 구속을 만든다. Rocks는 물리 없는 body를 Jolt sensor·비활성으로 두므로(3.4절) 잠금이 효과가 없다 — 같은 결과다.
- 물리 설정 `defaultDegreesOfFreedom`(`SettingsDof { Full3D, YZPlane, XZPlane, XYPlane }`, 기본 Full3D — 6.8절).
- 실행 중 바꾸는 setter(UE `SetDOFLock`)는 PH5c(body 값 setter)다. 결합(PC07 weld) body의 잠금은 PH6 결합 때 부모 body 설정을 따른다.

**시험**(Jolt body를 직접 읽으므로 Monolithic — `PhysicsDofSceneTests.cpp`):
- `ResolveDofLock` bit: SixDOF 축 잠금 여섯이 저마다 축 A의 bit 하나만 지운다(회전 잠금이면 gyroscopic 끔). 모두 잠금은 6축 구속이다. `XZPlane`은 Y 이동·X·Z 회전이다. `CustomPlane` 축 법선(0, 0, −3)은 표지마다 bit(이동 → Z 이동, 회전 → X·Y 회전), 법선 0·표지 없음은 잠금 없음, 비스듬한 법선은 구속(정규화한 법선)이다.
- SixDOF X 이동 잠금: X·Y 속도를 준 구가 Y로만 움직인다. 회전 잠금: 잠근 축 둘레 각속도가 0이다.
- `XYPlane`: 중력이 있어도 Z에 머물고(법선 이동 잠금), Z축 둘레 회전만 남는다. 표지(`lockTranslation` 거짓)와 상관없이 잠근다.
- `Default` + 설정 `YZPlane`: X 이동이 잠긴다.
- `CustomPlane`(법선 (1, 1, 0)/√2, 두 표지 참): 법선 방향으로 움직이지 않고 평면 안으로는 움직인다. 법선 둘레 각속도는 남고 평면 안 축 둘레는 막힌다. 표지가 거짓이면 구속이 없다.
- 모두 잠금: 중력·속도에도 제자리다(구속 1).
- Kinematic은 잠금이 없고(잠긴 축으로 옮겨진다), Dynamic으로 바꾸면 잠긴다. Dynamic → Kinematic → Dynamic에서 CustomPlane 구속 수가 1 → 0 → 1이고 body를 지우면 0이다. Kinematic에 남은 잠긴 축 속도는 전환 때 0이 된다.
- teleport한 CustomPlane body는 새 자리를 기준으로 잠긴다. scale 변경 뒤에도 축 bit 잠금이 남는다.
- Static은 자유도 설정이 있어도 구속이 없다.

**리뷰 반영**(치명 0, 중요 4, 경미 8): 모두 잠금을 무시하던 것을 UE처럼 6축 구속으로 바꿨다(경고·`AllDegreesOfFreedomLocked` 삭제). 전환 뒤 남은 속도를 새 마스크로 거른다. 구속 위치를 질량 중심에서 body 원점으로 바꾸고 teleport·형상 변경 때 다시 붙인다. 회전 bit 잠금에서 gyroscopic을 끈다. 축과 나란한 CustomPlane은 bit로 한다. 법선 0 판정을 성분마다(UE `IsNearlyZero`)로 맞췄다. `BodySettings.h` 주석(축 평면은 표지를 보지 않는다)과 시험 상수 이름을 고쳤다.
