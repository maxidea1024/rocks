# 커브 에셋 — 6. 소비자 연결

[목차](README.ko.md)

## 6. 소비자 연결

### 6.1 이 세션의 소비자(CA5)

| 소비자 | 지금 | UE |
|---|---|---|
| 진동 채널 `HapticChannel::curve`(InputActions 6.1절) | 인라인 `TangentCurve`. `HapticEffect::channels`는 `Array<HapticChannel>`(`HapticEffect.h:43`) | `FForceFeedbackChannelDetails::Curve` = `FRuntimeFloatCurve`(`RE/Classes/GameFramework/ForceFeedbackEffect.h:38`) |
| 입력 응답 커브 모디파이어 `responseX`·`responseY`·`responseZ`(InputActions 3장) | 인라인 `TangentCurve` 셋. 모디파이어는 `Array<InputModifier>`(`InputBinding.h:34`) 원소 | `UInputModifierResponseCurveUser::ResponseX/Y/Z` = `TObjectPtr<UCurveFloat>`(`EnhancedInput/Public/InputModifiers.h:332-336`) |

**제약:** 두 자리 모두 `Array` 원소 안의 값 struct다. 그래서 참조 struct(`FloatCurveSource` — 3.1절)를 그대로 넣을 수 없다. InputActions 2.7절도 같은 이유로 값 데이터에 `ObjectRef`를 쓰지 않는다고 정했다. 값 struct는 코드로 만든 효과·사본(IAD10, 6.2절)에 필요하므로 바꾸지 않는다.

**CA5 결정 후보.** 착수 때 하나로 정하고 이 절을 고친다.

1. 소유 에셋의 참조 표와 slot.
   - 소유 에셋(`HapticEffectAsset`·`InputContext`·`InputAction`)이 `ObjectRefArray<FloatCurveAsset> curveAssets`를 갖는다.
   - 값 struct의 커브 자리는 `{ TangentCurve curve; uint32 curveAsset = NoCurveAssetSlot; }`(값 — 복사 가능)다.
   - 사본을 만드는 곳(`CopyHapticEffect`·`CollectInputContext`)이 slot을 풀어 인라인으로 만든다(평탄화).
   - 코드로 만든 값은 slot이 비어 그대로 쓴다.
   - 편집기(IA6c)가 slot을 관리한다.
2. 소유 에셋 쪽 평행 구조.
   - 에셋 클래스가 채널·모디파이어마다 `StructRefArray<FloatCurveSource>`를 따로 가진다.
   - 구조가 두 번 생기고, 모디파이어는 중첩(RMC2304) 때문에 못 쓴다.
3. 들이지 않음.
   - 인라인만 둔다(◇ — UE 에셋 공유를 잃는다).

권장안은 1이다. UE 하드 참조와 같은 로드·쿠킹 의미를 갖고, 평가 경로는 지금처럼 Registry 없이 돈다. 버린 대안은 값 struct 안 `SoftObjectRef`다. 복사는 되지만 소유 에셋과 함께 로드되지 않아 사본을 만들 때 대상이 없을 수 있다.

### 6.2 다른 세션의 소비자(알림만 — 그 세션이 연결한다)

| 소비자 | UE 타입 | 담당 |
|---|---|---|
| 소리 감쇠 커브(사용자 감쇠·공기 흡수·리버브 송신·우선순위) | `FRuntimeFloatCurve`(`RE/Classes/Engine/Attenuation.h:107`, `Sound/SoundAttenuation.h:222-413`) | 오디오(rocks-be). 감쇠 설정이 값 struct면 6.1절과 같은 제약이 있다 |
| 후처리·노출 커브(노출 편향·지역 노출) | `TObjectPtr<UCurveFloat>`(`RE/Classes/Engine/Scene.h:595, 1959, 2054, 2061`) | 렌더(rocks-2e). 같은 제약이 있다 |
| 머티리얼 `CurveAtlasRowParameter` | `UCurveLinearColorAtlas` + 커브 | 머티리얼(rocks-2b) — 4장 |
| 타임라인 트랙(float·vector·color·event) | `TObjectPtr<UCurveFloat/UCurveVector/UCurveLinearColor>`(`RE/Classes/Components/TimelineComponent.h:81, 118, 156`) | 컴포넌트 CM5. World라 `SoftObjectRef` + lease를 쓴다(3.1절) |
| 게임 수치(`FScalableFloat` 대응) | `FCurveTableRowHandle`(GameplayAbilities 플러그인) | 게임플레이 능력 플러그인을 들일 때 |
| Niagara 커브 | 자체 `FRichCurve` + 편집기 전용 에셋 동기화 | VFX 단계 |

- 알림은 CA1(클래스·원천·값 struct)이 push된 뒤에 한다. 6.1절의 참조 제약과 CA5에서 고른 방식을 함께 알린다.
