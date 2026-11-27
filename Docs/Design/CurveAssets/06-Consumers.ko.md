# 커브 에셋 — 6. 소비자 연결

[목차](README.ko.md)

## 6. 소비자 연결

### 6.1 이 세션의 소비자(CA5)

| 소비자 | 지금 | UE |
|---|---|---|
| 진동 채널 `HapticChannel::curve`(InputActions 6.1절) | 인라인 `TangentCurve`. `HapticEffect::channels`는 `Array<HapticChannel>`(`HapticEffect.h:43`) | `FForceFeedbackChannelDetails::Curve` = `FRuntimeFloatCurve`(`RE/Classes/GameFramework/ForceFeedbackEffect.h:38`) |
| 입력 응답 커브 모디파이어 `responseX`·`responseY`·`responseZ`(InputActions 3장) | 인라인 `TangentCurve` 셋. 모디파이어는 `Array<InputModifier>`(`InputBinding.h:34`) 원소 | `UInputModifierResponseCurveUser::ResponseX/Y/Z` = `TObjectPtr<UCurveFloat>`(`EnhancedInput/Public/InputModifiers.h:332-336`) |

**제약:** 두 자리 모두 `Array` 원소 안의 값 struct다. 그래서 참조 struct(`FloatCurveSource` — 3.1절)를 그대로 넣을 수 없다. InputActions 2.7절도 같은 이유로 값 데이터에 `ObjectRef`를 쓰지 않는다고 정했다. 값 struct는 코드로 만든 효과·사본(IAD10, 6.2절)에 필요하므로 바꾸지 않는다.

**CA5 결정(2026-11-27, 권장안 1을 다듬음):** 소유 에셋의 참조 목록과 값 struct의 slot 번호다. 세부는 [InputActions](../InputActions/02-DataModel.ko.md) 2.9절(IA9, IAD17)이다.

- 소유 에셋(`InputAction`·`InputContext`·`HapticEffectAsset`)이 `ObjectRefArray<FloatCurveAsset> curveAssets`를 갖는다.
- 값 struct는 기존 인라인 커브 옆에 `uint32` slot 필드를 더한다(`responseXAsset`·`responseYAsset`·`responseZAsset`, `curveAsset` — 기본 `NoCurveAssetSlot`). 커브를 감싸는 새 struct를 두지 않아 평가 코드가 그대로다.
- 사본을 만드는 곳(`CollectInputContext`·`CopyHapticEffect`)이 공용 도우미 `TryResolveCurveSlot`(CurveAssets `CurveAssetSlot.h`)으로 평탄화한다(에셋 커브를 인라인에 복사, slot 비움).
- 평탄화되지 않은 slot은 평가기 검증이 `UnresolvedCurveAsset`으로, 진동 재생이 `InvalidArgument`로 거부한다.
- 버린 대안: 평행 `StructRefArray<FloatCurveSource>`(모디파이어 중첩 RMC2304), 인라인만(UE 에셋 공유를 잃는다), 값 struct 안 `SoftObjectRef`(함께 로드되지 않는다).

### 6.2 다른 세션의 소비자(알림만 — 그 세션이 연결한다)

| 소비자 | UE 타입 | 담당 |
|---|---|---|
| 소리 감쇠 커브(사용자 감쇠·공기 흡수·리버브 송신·우선순위) | `FRuntimeFloatCurve`(`RE/Classes/Engine/Attenuation.h:107`, `Sound/SoundAttenuation.h:222-413`) | 오디오(rocks-be). 감쇠 설정이 값 struct면 6.1절과 같은 제약이 있다 |
| 후처리·노출 커브(노출 편향·지역 노출) | `TObjectPtr<UCurveFloat>`(`RE/Classes/Engine/Scene.h:595, 1959, 2054, 2061`) | 렌더(rocks-2e). 같은 제약이 있다 |
| 머티리얼 `CurveAtlasRowParameter` | `UCurveLinearColorAtlas` + 커브 | 머티리얼(rocks-2b) — 4장 |
| 타임라인 트랙(float·vector·color·event) | `TObjectPtr<UCurveFloat/UCurveVector/UCurveLinearColor>`(`RE/Classes/Components/TimelineComponent.h:81, 118, 156`) | 컴포넌트 CM5 — [엔진 런타임](../EngineRuntime/32-TimelineComponent.ko.md) 32장(이 세션). 트랙은 인라인 값 + `SoftObjectRef` 에셋의 로드 사본(ERD71) |
| 게임 수치(`FScalableFloat` 대응) | `FCurveTableRowHandle`(GameplayAbilities 플러그인) | 게임플레이 능력 플러그인을 들일 때 |
| Niagara 커브 | 자체 `FRichCurve` + 편집기 전용 에셋 동기화 | VFX 단계 |

- 알림은 CA1(클래스·원천·값 struct)이 push된 뒤에 했다. CA5의 slot 방식(`TryResolveCurveSlot`)은 값 struct에 커브 에셋이 필요한 다른 소비자(소리 감쇠 등)도 같이 쓸 수 있다.
