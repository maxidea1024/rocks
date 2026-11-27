# 커브 데이터 — 키 편집·키 핸들

[목차](README.ko.md)

## 7. 키 편집 연산

값 타입의 공개 편집 API는 키 index와 `Result`로 동작한다(키 핸들은 8절의 표가 index 위에 얹는다). 모든 연산은 실패하면 커브를 바꾸지 않는다.

### 7.1 연산 표

| 연산 | 규칙(UE 대응) |
|---|---|
| `TryAddKey(time, value, unwindRotation = false) -> Result<usize>` | 같은 시간 키가 있으면 그 키들 **앞**에 넣는다(UE `AddKey` RC 303–339 — 첫 `keys[i].time >= time` 앞). 새 키는 Linear·Auto·가중 없음. `unwindRotation`이면 이전 키 값 기준으로 차이가 180°를 넘는 동안 360°씩 옮긴다(도 단위 — UE와 같다, 13절). 비유한 시간은 InvalidArgument |
| `TryUpdateOrAddKey(time, value, unwind, tolerance = 1e-4f)` | `|키 시간 − time| <= tolerance`인 첫 키의 값만 바꾸고(unwind는 새 키에만 — UE와 같다, RC 376–400) 없으면 추가 |
| `TryRemoveKey(index)` | 지운다(접선 재계산 없음 — D10) |
| `TrySetKeyTime(index, time) -> Result<usize>` | 키의 모든 칸을 유지한 채 새 시간 자리로 옮기고 새 index를 돌려준다(같은 시간 키 앞). 다른 키의 접선도 바꾸지 않는다 |
| `SetKeyValue`·`SetKeyInterp`·`SetKeyTangentMode`·`SetKeyTangentWeight`·`SetKeyTangents` | 칸만 바꾼다(index 범위 밖은 InvalidArgument인 `Try*` 판) |
| `TrySetKeys(Span<const Key>)` | 정렬·유한 시간 입력만 받는다(위반은 InvalidArgument — UE는 정렬을 기대만 하고 확인하지 않는다) |
| `FindKey(time, tolerance = 1e-4f) -> usize` | 이진 탐색, 없으면 `IndexNone`. 허용오차 안의 키가 여럿이면 **가장 앞** 키(UE `GetKeyIndex` RC 431–457은 탐색 경로에 따라 정해진다 — 15절 6) |
| `IsConstant(tolerance = 1e-8f)` | 키 ≤ 1이거나, 모든 키 값이 첫 키와 tolerance 안이고 Cubic 키가 없음(UE와 같다, RC 485–502) |
| `TryGetTimeRange() -> Result<Interval<float>>` | 키가 없으면 NotFound(UE는 (0, 0)) |
| `TryGetValueRange() -> Result<Interval<float>>` | 키 범위 안에서 Evaluate가 내는 값을 모두 담는 범위를 구간마다 **해석적으로** 구한다(UE는 표본 재귀 `FeaturePointMethod`, RC 604–670) — 7.3절, D11. 키가 없으면 NotFound, 범위가 유한하지 않으면 InvalidArgument |
| `ShiftTime(delta)`·`TryScaleTime(origin, factor)` | 모든 키 시간을 바꾼다(factor > 0만 — 순서 유지, 0·음수·비유한은 InvalidArgument, 결과 시간이 비유한이면 InvalidArgument). UE는 키마다 지우고 다시 넣는다(O(n²)) |
| `TryShiftKeyTimes(Span<const usize> indices, delta)` | 고른 키만 옮긴다(UE `ShiftCurve(Delta, KeyHandles)` 대응 — 편집기 선택 이동). 고른 키를 빼고, 시간을 바꿔, 원래 index 순서대로 "같은 시간 키 앞" 규칙으로 다시 넣는다. UE는 `TSet` 순회 순서라 결과 순서가 정해지지 않는다. 중복·범위 밖 index는 InvalidArgument |
| `TryBake(interval, first, last)` | `first + interval`부터 `last` 미만까지 간격 `interval`로 Evaluate한 값을 `TryUpdateOrAddKey`로 넣는다. 시간은 누적 덧셈이 아니라 `first + i × interval`로 계산한다(UE는 float 누적, 그리고 `first == last`면 `Reserve(−1)`이 fatal — RC 900–908, 15절 7). 이름도 UE `SampleRate`(실제로는 간격)를 `interval`로 바로잡는다 |
| `TryRemoveRedundantKeys(tolerance, Optional<FrameRate> sampleRate)`·범위판 `(tolerance, first, last, Optional<FrameRate>)` | 7.2절 |
| `Reset()` | 키만 비운다(기본값·외삽 유지 — UE와 같다) |

`ReadjustTimeRange`(UE 시퀀서 시간 범위 맞춤)는 에셋·도구 층의 일이라 값 타입에 두지 않는다(D12). 계단 커브의 `RemoveRedundantKeys`는 UE와 달리 남은 키 하나를 기본값으로 바꾸지 않는다(15절 9).

### 7.2 중복 키 제거 — UE `RemoveRedundantKeysInternal`(RC 966–1102), D20

- 알고리즘은 UE를 옮긴다: 시작·끝 보존 키 사이의 키마다, 표본판이면 직전 보존 키와 다음 키 사이를 `sampleRate`의 프레임마다 평가해 "그 키를 뺀 값"과 "있는 값"의 차가 tolerance를 넘으면 남긴다(RC 1024–1051). 표본판이 아니면 세 키가 모두 User·Break가 아닐 때만 그 키 시간에서 비교한다(RC 1053–1067).
- 표본 시간은 UE와 같은 `FrameRate::AsFrameTime(double)`(내림 + subframe, UE `FrameRate.h` 240–255)으로 시작해 frame 번호를 1씩 올리고 `AsSeconds`(같은 파일 232–238)를 float로 바꿔 평가한다. 그래서 D20의 `FrameRate`·`FrameNumber`·`FrameTime`이 선행이다. UE `FFrameRate(0, 0)`(무효 = 표본 없음)은 `Optional<FrameRate>`의 unset이다.
- 범위판의 비교 기준(UE `MostRecentKeepKeyIndex`)은 시작 보존 키에서 시작한다(UE는 index 0, RC 1017 — 15절 8).
- UE는 제거 뒤 `AutoSetTangents`를 부른다(RC 1094). Rocks는 D10에 따라 부르지 않는다. 그래서 Auto 키의 접선은 지운 이웃을 기준으로 계산된 옛 값이 남고, UE와 결과를 맞추려면 호출자가 이어서 `AutoSetTangents`를 부른다. UE 알고리즘이 Auto 키를 지워도 된다고 보는 것도 이 재계산을 전제한 판단이므로 편집 도구는 두 호출을 한 묶음으로 쓴다.

### 7.3 값 범위 — D11

- 구간마다 Evaluate가 그 구간에서 실제로 내는 값(구간 끝은 극한)을 넣는다: Linear는 두 끝, Constant는 앞 키 값(마지막 구간은 끝 키 값도), Cubic은 극값과 쓰이는 끝, 길이 0 구간은 마지막 구간일 때만 앞 키 값이다. 쓰이지 않는 키 값은 넣지 않는다. 외삽은 보지 않는다(UE와 같다).
- 비가중 Cubic: Bezier 값 다항식 y(s)의 도함수(2차)의 (0, 1) 안 근에서 `BezierCurveValue`로 평가한다.
- 가중 Cubic: 시간 축 x(s)와 값 축 y(s)가 모두 Bezier다. Evaluate가 쓰는 매개변수는 "x(s) = α의 [0, 1] 안 가장 큰 근"이므로 범위는 그 근들이 지나는 매개변수 집합 S 위의 y 극값이다.
  - x가 [0, 1]에서 단조(x′의 근이 (0, 1)에 없음)면 S = [0, 1].
  - 일반적으로 S = {s ∈ [0, 1] : x(s) ≥ 0이고 모든 u ∈ (s, 1]에서 x(u) > x(s)}(α = 1인 s = 1은 마지막 구간에서만 쓰인다)다. x′의 (0, 1) 안 근으로 나눈 조각 중 x가 증가하는 조각에서, 오른쪽 조각들의 x 최솟값보다 작은 부분이다. 두 근 s₁ < s₂(극대 x(s₁), 극소 x(s₂))이면 S = {s ∈ [0, s₁] : 0 ≤ x(s) < x(s₂)} ∪ {s ∈ [s₂, 1] : x(s) ≥ 0}이 된다. 조각 경계(뛰는 점)에서는 두 쪽 극한을 모두 넣는다.
  - Evaluate의 3차 방정식 풀이(UE `SolveCubic`, CE 32–35)는 판별식 |D| ≤ 1e-8이면 근을 겹근으로 근사해 S 밖의 매개변수를 고를 수 있다(얕은 접힘은 실근 셋인 α 구간 전체가 이 띠에 든다 — 무작위 탐색에서 수학적 범위를 폭의 2%까지 넘었다). 그래서 범위는 그 띠의 α에서 풀이가 실제로 고르는 매개변수도 넣는다(17.4절 C10).
  - S의 각 조각 안에서 y′의 근과 조각 끝에서 y를 평가한다. 근 계산은 double이다.
- 결과는 Evaluate가 키 범위 안에서 내는 값을 모두 담는다(float 반올림 여유만큼 — 계약 주석에 적는다). UE 표본 재귀와 결과가 다를 수 있다(15절 18).

## 8. 키 핸들

UE 사실(`KeyHandle.h`·`KeyHandle.cpp`): `FKeyHandle`은 32비트이고 전역 `std::atomic<uint32>` 카운터에서 발급되며 0이 무효다(첫 값 2). 핸들 표는 커브 안의 `UPROPERTY(transient) mutable` 칸이라(`IndexedCurve.h` 98–99) 디스크에 저장되지 않고 undo 버퍼에만 기록된다(`KeyHandle.cpp` 144–154). 핸들은 필요할 때 늦게 만들어지고, 키 배열을 직접 고치면 표와 어긋나 개수만 맞춘다. 커브를 복사하면 표가 비고(새 핸들), **대입하면 대상의 옛 표가 그대로 남아** 옛 핸들이 새 커브의 같은 index 키를 가리킨다(KH 74–76 — 결함). `const` 조회가 `mutable` 표를 고쳐 const 동시 조회가 경쟁한다.

Rocks(D3):

- 키 핸들은 **값의 일부가 아니다**. 값 타입(`TangentCurve` 등)은 핸들을 갖지 않고, 복사·비교·직렬화·차이 저장에 나타나지 않는다.
- **표와 커브의 연결**: 표는 커브를 가리키는 포인터·참조를 쥐지 않는다(커브는 객체 필드·배열 원소 안에서 옮겨 다니므로 주소를 붙잡을 수 없다). 표의 모든 함수가 커브를 인자로 받는다: `table.TryAddKey(curve, time, value)`, `table.TryFindIndex(curve, handle) -> Result<usize>`, `table.TryRemoveKey(curve, handle)`, `table.TrySetKeyTime(curve, handle, time)` 등. 표를 거치는 편집 함수가 커브와 표를 함께 갱신한다.
- **표를 거치지 않은 수정의 감지**: 표는 마지막으로 맞춘 키 배열 전체의 사본(키 struct 그대로, 비트 비교)을 쥔다. 표의 모든 함수는 먼저 인자로 받은 커브의 키 배열을 사본과 비교하고(O(N)), 다르면 표 전체를 무효로 표시하고 StaleHandle을 낸다. 무효가 된 표는 `TryRebuild(curve)`가 새 핸들을 다시 발급할 때까지 모든 조회가 StaleHandle이다(UE처럼 끝 쪽만 맞춰 다른 키를 가리키게 두지 않는다). 리플렉션 경로 쓰기·대입 묶음·undo가 값을 바꿔도 같은 방법으로 잡힌다. 해시가 아니라 사본 비교라 충돌로 놓치는 일이 없다. 비용은 편집기에서만 든다(메모리는 키 배열 하나 더, 시간은 표 호출마다 O(N)).
- 핸들 값은 `uint32`이고 표마다 1부터 단조 증가하며 0은 무효(`CurveKeyHandle::Invalid`)다. 전역 카운터를 쓰지 않는다 — 핸들은 그 표 안에서만 뜻이 있고, 표마다 세면 결정적이다(시험·재실행에서 같은 값). 표의 카운터가 `NumericLimits<uint32>::Max()`에 닿으면 새 핸들 발급이 Overflow다(UE는 쿡 중에 0을 건너뛰며 순환).
- 표 복사는 핸들·사본까지 그대로 복사한다(복제된 편집 세션이 같은 키를 같은 핸들로 가리킨다). 대입도 마찬가지로 원본 표로 완전히 바뀐다.
- 표는 저장하지 않는다(UE와 같다). undo 기록은 표의 직렬 형태를 따로 쓴다(CU4 Undo 설계와 함께 정한다 — 그 단계의 일).
