# 커브 데이터 — InterpCurve 미러·좌표·에셋 층 경계

[목차](README.ko.md)

## 12. InterpCurve 미러와 값 범위 — D19

G09와 [키 커브](../../Implementation/InterpCurve.ko.md) 2절(`operator<<` → "리플렉션 미러·직렬화 단계", `CalcBounds` → 후속)이 이 단계로 미룬 일이다.

### 12.1 미러

- 대상은 UE에 USTRUCT가 있는 다섯 형식이다: `InterpCurve<float>`(UE `FInterpCurveFloat`), `InterpCurve<Vec2d>`(`FInterpCurveVector2D` — UE 5의 `FVector2D`는 double), `InterpCurve<Vec3d>`(`FInterpCurveVector`), `InterpCurve<Quatd>`(`FInterpCurveQuat`), `InterpCurve<LinearColor>`(`FInterpCurveLinearColor`)와 각 `InterpCurvePoint<T>`. `FInterpCurveTwoVectors`는 Rocks 값 형식이 없어 두지 않는다. 다른 형식(`double`·float 벡터 등)은 소비자가 생길 때 미러를 더한다.
- `InterpCurvePoint<T>`는 멤버가 public(`inValue`·`outValue`·`arriveTangent`·`leaveTangent`·`mode`)이라 지금 Binds 규칙("공개 데이터 멤버를 같은 이름·형식·선언 순서로" — ReflectionExtensions 3.2절)으로 미러한다. `Atomic`이다(키 struct와 같은 이유). `InterpCurveMode` enum의 미러가 필요하다(Core enum Binds는 없어서 CV9에서 더했다 — 17.8절 I2).
- `InterpCurve<T>`는 키 배열 `points_`·`looped_`·`loopKeyOffset_`이 private이다(정렬 불변식). 접근자로 미러하지 않고 **private 멤버 접근을 기반에 보강**한다: Core 쪽 `InterpCurve<T>`가 `template <class> friend struct ::rocks::GeneratedType;`을 선언한다(Core는 `GeneratedType`을 앞 선언만 하므로 의존이 생기지 않는다 — `ROCKS_GENERATED_BODY`가 넣는 friend와 같은 것, `Meta/Annotations.h` 101–105). MetaCompiler는 Binds 미러 멤버가 실제 타입의 private 멤버여도 이 friend를 통해 접근하는 생성 코드를 낸다. 미러 멤버 이름은 C++ 이름(`points_` 등)이고 도구·JSON 이름은 `Name="points"`·`"looped"`·`"loopKeyOffset"`이다.
- 미러는 `Validate`를 둔다(9.1절): 키 입력값 비내림차순·유한, `loopKeyOffset` 유한, `looped`이면 offset이 유한한 양수다(키 없는 루프 커브도 받는다 — `Reset`·`TryRemovePoint`가 남기는 값, 17.8절 I4).
- 생성·복사: `InterpCurve<T>`는 `Allocator&` 생성자와 `TryCopyFrom`이 있어 `RuntimeStructOps`의 construct·copy를 채울 수 있다. 같음은 리플렉션 값 기준이다(UE `operator==`는 루프가 꺼져 있으면 offset을 비교하지 않는다 — 리플렉션 같음은 모든 칸).

### 12.2 `TryCalcBounds`

- `Result<CurveBounds<T>> TryCalcBounds() const`(최소·최대 한 쌍). 키가 없으면 NotFound(UE는 인자로 받은 기본값 — `InterpCurve.h` 746–754).
- float·double·`Vec2/2d`·`Vec3/3d`·`LinearColor`(성분별)만 지원하고 quaternion·`Vec4`는 컴파일 시점에 막는다. UE도 일반 형식(`FQuat` 등)은 아무것도 하지 않는 빈 함수다(`InterpCurvePoint.h` 406–408).
- 구간마다 UE `FindBounds`(`Core/Private/Math/UnrealMath.cpp` 1512–1573)를 같은 식·같은 순서로 옮긴다: 두 끝 값의 min·max, 곡선 키면 접선에 구간 길이를 곱해 Hermite 도함수 계수 `a = 6P0 + 3T0 + 3T1 − 6P1`, `b = −6P0 − 4T0 − 2T1 + 6P1`, `c = T0`, 근이 구간 안(`t > StartT && t < EndT`)이면 `CubicInterp` 값을 넣는다.
- **UE와 다른 점**: UE는 루프 커브의 닫는 구간(마지막 키 → 첫 키)에 첫 키의 입력값을 끝 시간으로 넘겨(`InterpCurve.h` 765–770) 구간 길이가 음수가 되고 극값을 보지 않는다. Rocks는 닫는 구간의 끝 시간을 `마지막 키 + loopKeyOffset`, 구간 길이를 `loopKeyOffset` 그대로(InterpCurve 평가와 같은 길이 — 끝 시간에서 시작 시간을 빼면 float 반올림으로 달라질 수 있다) 둔다(15절 22).

## 13. 좌표·단위·각도

- 스칼라 커브 자체에는 단위가 없다. 커브를 묶는 에셋·애니메이션 타입이 UE 5.8 규약을 그대로 따른다(절대 규칙): 벡터 커브는 성분 X·Y·Z(cm 등 UE와 같은 단위), 변환 커브의 회전 성분은 **도 단위 오일러이고 X = Roll, Y = Pitch, Z = Yaw**(UE `FTransformCurve` — `AnimCurveTypes.cpp` 283·294–297, 평가는 `FRotator(Pitch = Y, Yaw = Z, Roll = X)` → quaternion), 색 커브는 RGBA 4성분이다. Rocks `Rotator`의 필드·적용 순서가 이미 UE와 같다.
- `unwindRotation`(7절)은 도 단위 ±180° 보정이다(UE와 같다, RC 319–334).
- `InterpCurve<Vec3d>`·`InterpCurve<Quatd>` 미러(12절)는 UE `FInterpCurveVector`·`FInterpCurveQuat`와 같은 좌표·단위를 그대로 담는다(변환 없음).
- 이 값 타입 층에는 벡터·회전 키 커브 묶음이 없다. 그 묶음 타입은 14절의 에셋 층에서 만든다.

## 14. 에셋 층 경계(범위 밖)

A1 에셋 시스템 뒤 A 단계에서 아래를 만든다. 이 문서는 그것들이 이 층에 기대는 방식만 정한다.

| UE 5.8 | Rocks에서 이 층과의 연결 |
|---|---|
| `UCurveFloat`(`FRichCurve FloatCurve`) | 에셋 객체가 `TangentCurve` 필드 하나를 갖는다 |
| `UCurveVector`·`UCurveLinearColor` | `TangentCurve` 성분 3·4개(13절 규약) |
| `FRuntimeFloatCurve`(인라인 커브 + 외부 에셋 참조) | 인라인 `TangentCurve` + `SoftObjectRef` 또는 `ObjectRef` 하나 — 참조를 고르는 규칙은 에셋 층이 정한다 |
| `UCurveTable`(`TMap<FName, FRealCurve*>`, 표 전체가 Simple·Rich 모드 하나) | 저장 형태는 에셋 층이 정한다(D2). 후보는 표 단위 모드 + 타입이 정해진 맵 하나(`OrderedMap<Name, TangentCurve>` 또는 `OrderedMap<Name, PlainCurve>`)와 행마다 `InstancedStruct<ScalarCurveBase>`다. UE 표는 행 타입이 섞이지 않으므로 앞쪽이 lease·힙 블록·Record 머리말 비용이 없다 |
| `FCurveTableRowHandle` | 표 참조 + 행 Name |
| `FVectorCurve`·`FTransformCurve`(애니메이션) | 애니메이션 단계 |

다형 저장을 고르는 경우의 평가 규칙(D2): 자유 함수 `TryEvaluateScalarCurve(const InstancedStruct<ScalarCurveBase>&, time, fallback) -> Result<float>`가 담긴 타입이 `TangentCurve`·`PlainCurve`면 평가하고, 비어 있으면 NotFound, `ScalarCurveBase` 자체나 이 함수가 모르는 파생 타입(게임 모듈이 만든 파생 등)이면 Unsupported다. 기본값이나 0으로 조용히 대신하지 않는다. virtual이 없으므로 새 파생 타입을 평가하려면 이 함수의 분기를 넓힌다.
