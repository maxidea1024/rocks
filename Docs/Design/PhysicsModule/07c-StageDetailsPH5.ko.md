# 물리 모듈 — 7.1 단계별 구현 세부(PH5)

[목차](README.ko.md) · [7. 단계·시험·결정](07-StagesDecisions.ko.md) · [PH4](07b-StageDetailsPH4.ko.md)

7장 단계 표(7.1절)의 PH5 하위 단계 구현 세부다. 절 번호는 7장 그대로다.

### 7.1.15 PH5 하위 단계와 PH5a 구현 세부(2026-11-27)

PH5는 넷으로 나눈다(크기 — "순서대로"). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH5a | body 값 사상의 나머지(6.5절 — 질량·관성·질량 중심, 감쇠 외 속도 상한, solver 반복, gyroscopic, smooth edge), 기본 물리 재질·결합 방식·반발 문턱(6.6절), 상자 margin, 물리 설정 칸(6.8절) | PH4 |
| PH5b | DOF(6.5절 — 축 잠금·평면·CustomPlane `SixDOFConstraint`·설정 기본 DOF·모두 잠금 경고) | PH5a |
| PH5c1 | 힘·충격량·토크·방사 힘과 속도 API(2.6절 — PA04·PA05를 컴포넌트 경로로, 7.1.17절) | PH5a |
| PH5c2 | 질량 API(`GetMass`·`SetMassOverrideInKg`·`SetMassScale`·`GetCenterOfMass`·`SetCenterOfMass`·`GetInertiaTensor`), 각속도 상한 API, body 값 setter(중력·감쇠·CCD·DOF·질량 다시 계산 표지) | PH5c1 |
| PH5d | 잠듦·깨움 API·이벤트(2.7절 — `OnComponentWake`·`OnComponentSleep`, `generateWakeEvents`)와 `OnComponentPhysicsStateChanged` | PH5c2 |

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

### 7.1.17 PH5c1 구현 세부(2026-11-28)

힘·충격량·토크·방사 힘과 속도 API다(2.6절). UE `UPrimitiveComponent`(`E/Private/PrimitiveComponentPhysics.cpp:146–490`) → `FBodyInstance`(`E/Private/PhysicsEngine/BodyInstance.cpp:3631–3660`·`3769–3906`·`4214–4232`) → Chaos(`PhysicsCore/Private/ChaosEngineInterface.cpp:846–1110`, 외부 handle `CH/Public/PhysicsProxy/SingleParticlePhysicsProxy.h:706–880`·`322–340`)를 따른다.

**컴포넌트 API**(`PrimitiveComponent` — 이름은 UE 그대로, 뼈 이름 인자는 CM7 때). 단위는 cm·kg·초, 각은 rad(`…InDegrees`는 도를 rad로 바꿔 같은 경로):
- 충격량: `AddImpulse(impulse, velocityChange)`, `AddAngularImpulseInRadians/InDegrees(impulse, velocityChange)`, `AddImpulseAtLocation(impulse, location)`, `AddVelocityChangeImpulseAtLocation(velocityChange, location)`, `AddRadialImpulse(origin, radius, strength, falloff, velocityChange)`.
- 힘: `AddForce(force, accelerationChange)`, `AddForceAtLocation(force, location)`, `AddForceAtLocationLocal(force, location)`, `AddTorqueInRadians/InDegrees(torque, accelerationChange)`, `AddRadialForce(origin, radius, strength, falloff, accelerationChange)`.
- 속도: `SetPhysicsLinearVelocity(velocity, addToCurrent)`, `GetPhysicsLinearVelocity()`, `GetPhysicsLinearVelocityAtPoint(point)`, `SetPhysicsAngularVelocityInRadians/InDegrees(velocity, addToCurrent)`, `GetPhysicsAngularVelocityInRadians/InDegrees()`. body가 없으면 읽기는 0이다.
- 방사 무시 표지 `ignoreRadialImpulse`·`ignoreRadialForce`(UE `bIgnoreRadialImpulse`·`bIgnoreRadialForce`, 기본 거짓 — 영속 필드, `SetIgnoreRadialImpulse`·`SetIgnoreRadialForce`): 참이면 방사 호출을 무시한다.
- `RadialFalloff { Constant, Linear }`(UE `ERadialImpulseFalloff`).

**판정·깨움(UE 그대로).**
- 힘·충격량·토크는 Dynamic body에만 효력이 있다(UE `IsBodyDynamic` — Static·Kinematic은 무시). body가 없으면 무시한다.
- 효력이 있으면 언제나 깨운다(Chaos 외부 handle의 `AddForce`·`AddTorque`·`SetLinearImpulse`·`SetAngularImpulse`가 값과 상관없이 `Dynamic` 상태로 바꾼다). 방사는 범위 안일 때만이다.
- 속도 setter는 Dynamic에만 효력이 있다. UE는 Kinematic에도 값을 넣지만(`IsRigidBody`) Chaos Kinematic은 목표가 없으면 속도로 움직이지 않는다(`PBDRigidsEvolutionGBF.cpp:1187` 모드 None). Jolt Kinematic은 속도로 움직이므로 Rocks는 Kinematic을 무시한다(되읽기만 다르다 — 한계). 잠든 body는 최종 속도가 거의 0이 아니면(성분마다 `KindaSmallNumber` 초과 — UE `SetVBase`의 `IsNearlyZero`) 깨운다. 최종 속도는 허용 자유도로 거르고 속도 상한으로 자른다(`Set…VelocityClamped`).

**충격량은 다음 step에 더한다(UE).** Chaos는 충격량을 호출 때 질량·월드 역관성으로 속도 변화로 바꿔 입자의 impulse velocity에 쌓고 다음 step 적분 때 속도에 더한다. 그래서 `AddImpulse` 바로 뒤 `GetPhysicsLinearVelocity`는 옛 속도이고, 같은 프레임의 `SetPhysicsLinearVelocity` 뒤에도 쌓인 몫이 더해진다. Rocks도 칸에 대기 속도 변화(Jolt 단위 Δv·Δω)를 쌓고(호출 때 Jolt 역질량·`MultiplyWorldSpaceInverseInertiaByVector`로 바꾼다), Step이 Jolt Update 직전(결정적 FP 환경 안)에 `Set…VelocityClamped(지금 + Δ)`로 더한다. body를 지우거나 Kinematic으로 바꾸면 대기 몫을 버린다(Kinematic은 forces도 지운다). 대기 목록 할당이 실패하면 그 자리에서 더한다(할당 실패 한정).
- `velocityChange`: 선형 Δv = 값, 각 Δω = 값(UE `AddVelocity`·`AddAngularVelocityInRadians` — 질량·관성을 곱했다 나누므로 같다).
- 위치 충격량: Δv = J/m, 각충격량 (위치 − 월드 질량 중심) × J. `AddVelocityChangeImpulseAtLocation`은 J = m·Δv(UE).
- 방사 충격량(UE `AddRadialImpulse`): d = 질량 중심 − origin, |d| < radius일 때만(엄격). |d| > 0이면 방향 d/|d|, Constant면 strength, Linear면 strength × (radius − |d|)/radius. |d| = 0이면 (1, 0, 0) × strength. `velocityChange`면 속도 변화다.

**힘·토크는 프레임의 고정 스텝마다 작용한다(UE `bAllowSubstepping`).** UE `UPrimitiveComponent::AddForce`는 `bAllowSubstepping` 참으로 부르고(substep마다 같은 힘), 기본은 프레임 dt 한 step이다. Rocks는 고정 스텝이라 한 프레임에 스텝이 0–상한 회다(3.5절). 그래서:
- 고정 스텝 밖(Update 등 — `World::InFixedSteps()` 거짓)에서 준 힘·토크는 칸의 프레임 힘에 쌓아 그 프레임의 고정 스텝마다 Jolt 누적 힘에 다시 넣고, 결과 반영(PhysicsResults — `EndFrame`)이 지운다. 스텝이 둘이면 둘 다, 0이면 버린다. 프레임마다 다시 주는 지속 힘이 프레임 속도와 상관없이 같은 효과다(스텝마다 한 번).
- Fixed hook 안에서 준 힘은 다음 스텝 한 번이다(Jolt `Body::AddForce`·`AddTorque`는 Update 한 번 뒤 지워진다 — `PhysicsSystem.cpp:2506`).
- step하지 않는 장면(Editor·Preview)은 그 자리에서 Jolt 누적 힘에 넣는다.
- `accelerationChange`: 힘 × m(칸 질량), 토크 = 월드 관성 × 값(Chaos `GetWorldInertia` — 칸 관성을 body 회전으로 돌린 R·I·Rᵀ).
- 위치 힘: 토크 (위치 − 월드 질량 중심) × 힘. `…Local`은 위치·힘을 body 원점 transform(질량 중심 아님 — Chaos `GetActorWorldTransform`)으로 월드로 바꾼다.
- 방사 힘(UE `AddRadialForce`): |d| > radius면 무시(경계 포함 — 충격량과 다르다, UE 그대로). |d| < 1e-4면 방향 (1, 0, 0), Linear는 (radius − |d|)/radius × strength. `accelerationChange`면 × m.
- radius ≤ 0인 방사는 무시한다(UE Linear 힘이 0/0이 되는 경우를 피한다 — UE Constant·중심 일치·radius 0 한 점 경우는 +X·strength를 주지만 Rocks는 무시한다, 한계).

**잘못된 값.** 비유한 벡터·위치·radius·strength와 Jolt float로 바꾸면 넘치는 값(제곱 길이가 넘치는 값 포함 — Jolt 속도 자르기가 assert한다)은 무시하고 Warning `NonFiniteBodyInput`이다(Jolt에 NaN·inf를 넣지 않는다 — UE는 검사하지 않는다). 속도 setter의 상한 자르기는 바로 일어난다(UE는 다음 적분 때 — Jolt `SetLinearVelocity`가 상한 초과를 assert하므로 `…Clamped`를 쓴다, 되읽기 한계). 충격량·힘 변환은 다른 접점 변환처럼 게임 thread의 FP 환경에서 하고, 적분만 결정적 FP 환경 안이다.

**잘못된 호출 경고**(UE `WarnInvalidPhysicsOperations` — shipping·test 빌드 밖, `PrimitiveComponentPhysics.cpp:146–170`): 충격량·힘·토크 setter(방사 제외)는 먼저 Mobility를 본다(UE `CheckStaticMobilityAndWarn` — Rocks `SceneComponent::CheckMovable`, 게임 World 초기화 뒤 Movable이 아니면 Warning `StaticComponentMoved`). 그것을 통과하고 `simulatePhysics`가 거짓이면 `PhysicsOperationWithoutSimulation`, 충돌 켬이 NoCollision·QueryOnly면 `PhysicsOperationWithoutPhysicsCollision`이다. 속도 setter는 Mobility만 본다(UE가 body 인자 없이 부른다). UE처럼 호출마다 남긴다. 시뮬레이션·충돌 경고는 `ROCKS_BUILD_RELEASE`가 아닐 때만이다(UE shipping 대응 — Mobility 경고는 기존 `CheckMovable` 그대로).

**World 접점**(`PhysicsScene`): `ApplyBodyForce(body, BodyForce)`(종류 `BodyForceKind` — 위 아홉, 값·위치·radius·strength·falloff·질량 무관 표지) 하나와 `SetBodyLinearVelocity(body, v, addToCurrent)`·`SetBodyAngularVelocity(body, ω, addToCurrent)`. 없는 handle·옛 세대는 무시한다. `BodyKinematics`에 월드 질량 중심 `centerOfMass`(cm)를 더한다(점 속도 v + ω × (점 − 질량 중심) — UE `GetUnrealWorldVelocityAtPoint`, PH5c2 `GetCenterOfMass`도 쓴다).

**시험.**
- 장면(Monolithic — `PhysicsForceSceneTests.cpp`): 대기 충격량은 step 전에 속도에 보이지 않고 step 뒤에 Δv = J/m이다, 같은 프레임 속도 setter 뒤에도 더해진다, Kinematic 전환·body 삭제가 버린다. 각충격량 Δω = I⁻¹·L(구 2/5·m·r²). 위치 충격량의 각 몫. 방사 충격량(경계 엄격·Linear 비율·중심 일치 (1, 0, 0)), 방사 힘(경계 포함·Linear). 힘·토크 accelerationChange가 질량·관성에 무관한 가속. Local 위치 힘의 회전. Static·Kinematic 무시, 잠든 body 깨움, 속도 setter의 깨움 문턱·자유도 거르기. 비유한 입력 무시.
- 컴포넌트(`PhysicsForceTests.cpp`, World 경로): PA04 — 구에 `AddImpulse((0, 0, m·100))` → 바로 읽으면 옛 속도, 한 스텝 뒤 기준 구보다 100 cm/s(감쇠 배율) 빠르다(질량 m은 UE 규칙으로 계산 — 질량 덮어쓰기 API는 PH5c2, 2 kg 그대로의 PA04는 장면 시험), `AddForce((0, 0, m·980))`·`AddForce((0, 0, 980), true)`를 스텝마다 주면 중력 상쇄(높이 유지). PA05 — 반폭이 다른 상자(주축 관성 m·(h₁² + h₂²)/3)에 `AddTorqueInRadians(τ)` → 각가속도 τ/I, 비대칭 상자는 축마다 다른 가속, τ = 10⁴ kg·cm²/s²가 1 N·m로 들어간다. `…InDegrees` 변환, `ignoreRadial…`, 점 속도, 잘못된 호출 경고 셋, body 없음 0.

**PH5c1 리뷰 반영(2026-11-28).** 치명 0, 중요 2, 경미 7. 고정 스텝 밖에서 준 힘이 0스텝 프레임에 쌓이거나 다스텝 프레임의 첫 스텝에만 들어가던 것을 프레임 힘(UE `bAllowSubstepping`)으로 고쳤다(`World::InFixedSteps`, `BodyForce::everyFixedStep`, `EndFrame`). 구로만 보던 월드 관성·Local 힘 시험을 돈 비대칭 상자·회전축과 다른 힘 방향으로 바꿨다. Kinematic 전환의 힘 지우기 중복(Jolt가 한다)을 없앴고, float 넘침을 막았고, 범위 밖 방사가 깨우지 않음·방사 힘 중심 일치·경고 수 시험을 더했다. 속도 setter의 Kinematic 차이·상한 되읽기·radius 0 한계와 접점 사용 예를 적었다.

### 7.1.18 PH5c2 구현 세부(2026-11-28)

질량 API·각속도 상한 API·body 값 setter다(2.6절). UE `UPrimitiveComponent`(`E/Private/PrimitiveComponentPhysics.cpp:190–198`·`470–477`·`493–515`·`571–818`) → `FBodyInstance`(`BodyInstance.cpp:736–740`·`849–853`·`2182–2186`·`2961–2985`·`3387–3500`·`3568–3578`·`3668–3693`·`3951–3971`·`4010–4026`·`4055–4071`)를 따른다.

**컴포넌트 API**(이름은 UE 그대로, 뼈 이름 인자는 CM7 때). 값은 `bodySettings`(영속)를 바꾸고 body가 있으면 바로 넣는다. body가 없으면 값만 바뀐다(다음 생성이 쓴다 — UE `FBodyInstance` 값과 같다).
- 질량: `GetMass()`(kg — Static·body 없음 0, UE `GetMass_AssumesLocked`는 rigid만), `SetMassOverrideInKg(massInKg, overrideMass = true)`, `SetMassScale(scale)`·`GetMassScale()`, `GetCenterOfMass()`(월드 질량 중심 cm — body 없음 0), `SetCenterOfMass(offset)`(UE `COMNudge` — body 공간 cm), `GetInertiaTensor()`(kg·cm², body 축 대각 — UE `GetLocalInertiaTensor`는 주값이고 축 정렬 형상(상자·구·캡슐)에서 같다, 비대각 형상(convex·compound — PH6)은 고유값으로 바꿔야 한다, Static·body 없음 0). 질량 setter는 UE `UpdateMassProperties`처럼 7.1.15절 규칙으로 다시 계산한다(깨우지 않는다). 형상의 질량 중심 감싸기는 이동 값이 바뀐 때만 다시 넣는다(UE는 형상을 건드리지 않는다 — 형상 교체는 Jolt broadphase·접촉 cache를 새로 한다). 형상을 다시 만들지 못하면(할당 실패) 이전 질량을 두고 Warning `BodyMassUpdateFailed`다. Static은 값만 둔다(UE `IsRigidBody` 거짓).
- 각속도 상한: `SetPhysicsMaxAngularVelocityInDegrees/InRadians(value, addToCurrent = false)`(UE `SetMaxAngularVelocityInRadians(…, bUpdateOverrideMaxAngularVelocity 참)` — `overrideMaxAngularVelocity`를 켜고 `maxAngularVelocity`(도/s)에 넣는다, addToCurrent면 지금 실효 값에 더한다 — body가 없으면 더하지 않는다, UE는 physics actor가 있을 때만 옛 값을 읽는다 `:3677–3686`), `GetPhysicsMaxAngularVelocityInDegrees/InRadians()`(덮어쓰기면 그 값, 아니면 물리 설정 `maxAngularVelocity` — 장면이 없으면 Collision `DefaultMaxAngularVelocityDegrees`).
- 그 밖: `SetEnableGravity`·`IsGravityEnabled`(켜면 Dynamic body를 깨운다 — UE), `SetLinearDamping`·`GetLinearDamping`, `SetAngularDamping`·`GetAngularDamping`, `SetUseCCD`(Jolt `EMotionQuality::LinearCast`·`Discrete`), `SetGyroscopicTorqueEnabled`·`GetGyroscopicTorqueEnabled`, `SetConstraintMode(DofMode)`(UE `SetDOFLock` — 7.1.16절 잠금을 다시 풀어 넣는다, 잠금 표지는 그대로).
- 잘못된 호출 경고(7.1.17절 규칙): `SetMassOverrideInKg`·`SetMassScale`·`SetCenterOfMass`·`SetPhysicsMaxAngularVelocity…`는 Mobility만, `GetMass`는 body 경고까지(UE 그대로 — 시뮬레이션 꺼짐·물리 충돌 없음). 그 밖은 경고하지 않는다.
- 잘못된 값: 비유한 값과 음수 각속도 상한·음수 감쇠(Jolt assert — `MotionProperties.h:83·92·96`)·모르는 `DofMode`는 값을 바꾸지 않고 Warning `InvalidBodyValue`다(UE는 검사하지 않는다 — 편집기 ClampMin만). 음수 질량·배율은 7.1.15절 최소 질량 1 g이 받는다(UE와 같다).

**World 접점.** `UpdateBodySettings(body, settings, BodySettingsChange)`(바뀐 묶음 하나 — `Gravity`·`Damping`·`ContinuousCollision`·`MaxAngularVelocity`·`GyroscopicTorque`·`Mass`·`DegreesOfFreedom`)와 `DefaultMaxAngularVelocityDegrees()`(물리 설정 값). `BodyKinematics`에 `mass`(kg)·`inertia`(kg·cm², body 축 대각)를 더한다(칸의 UE 질량 속성 — 자유도 잠금과 무관, Static은 0).
- `Mass`: 칸 질량 입력을 바꾸고 형상(질량 중심 감싸기)·질량 속성을 다시 넣는다(Dynamic이면 자유도와 함께, 구속은 다시 붙인다). 깨우지 않는다.
- `DegreesOfFreedom`: `ResolveDofLock`을 다시 하고 구속을 떼었다 붙이며 gyroscopic 표지를 다시 정한다(`gyroscopicTorque` && 회전 bit 잠금 없음).
- `MaxAngularVelocity`: 덮어쓰기면 그 값, 아니면 장면 기본을 `MotionProperties::SetMaxAngularVelocity`에 넣는다.

**시험.**
- 장면(Monolithic — `PhysicsBodyValueSetterSceneTests.cpp`): 묶음마다 Jolt 값(중력 계수·감쇠·motion quality·각속도 상한·gyroscopic 표지·허용 자유도와 구속), 질량 다시 계산(override·scale·질량 중심 이동의 감싸기·관성), Static은 질량 그대로, 중력 켬이 잠든 body를 깨움, 질량 변경은 깨우지 않음.
- 컴포넌트(`PhysicsBodyValueTests.cpp`): PA04 정확 — `SetMassOverrideInKg(2)` 뒤 `GetMass()` 2, `AddImpulse((0, 0, 200))` → 한 스텝 뒤 Δv 100 cm/s(감쇠 배율). `GetInertiaTensor`가 구 2/5·m·r², `SetCenterOfMass`가 `GetCenterOfMass`를 옮김, 각속도 상한 덮어쓰기·더하기·기본, 중력 끔(떨어지지 않음), CCD·감쇠·`SetConstraintMode` 왕복, body 없음 기본값, 잘못된 값 거부.

**PH5c2 리뷰 반영(2026-11-28).** 치명 0, 중요 1, 경미 9. 시험 공백을 메웠다: 구속이 있는 body의 질량 중심 변경(구속 다시 붙임·잠금 유지), 질량 중심 변경 전후 원점 유지, 반폭이 다른 상자의 관성 세 성분, 회전 잠금 해제 뒤 gyroscopic 복귀·잠긴 body의 gyroscopic 요청, Kinematic은 중력을 켜도 깨지 않음, 덮어쓰기 아닌 상한에 더하기. 질량만 바뀌면 형상을 바꾸지 않고(접촉 cache 유지), 형상 실패를 경고하고 칸을 그대로 두며, body 없는 addToCurrent를 UE처럼 더하지 않게, 모르는 `DofMode`를 거르게 고쳤다. 관성이 텐서 대각(축 정렬 형상에서 주값)임을 적었다. 2.6절 표·주석 정리.

