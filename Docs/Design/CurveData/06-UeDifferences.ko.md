# 커브 데이터 — UE 5.8과 다른 점

[목차](README.ko.md)

## 15. UE 5.8과 다른 점

기본은 UE와 같은 의미·같은 비트(Windows x64 기준 — D17)다. 아래는 결함 수정(★)이거나 Rocks 규약(◇)이다.

1. ★ 순환 외삽: 키 시간이 모두 같거나(길이 0) 시간이 비유한이면 재배치하지 않는다. UE는 0 나눗셈과 Inf·범위 밖 값의 정수 변환이다(`RealCurve.cpp` 7–34, x64에서는 `cvtss2si` 무효값 → cycle 수 −2^30). cycle 몫이 2^30 이상이면 Rocks는 double 재배치(D16), UE는 SSE 식의 무의미한 값(UPM 86–91 — 예: 몫 −(2^30 + 128)에서 시간 −128)이다.
2. ★ Linear·Bezier의 `Lerp` 끝점: 비율 0·1에서 끝 값을 정확히 낸다(InterpCurve 4절 5와 같은 수정). Lerp·Bezier에 실제로 넣는 비율(가중 경로는 3차 방정식 근을 float로 바꾼 값 — 입력 비율이 0·1이 아니어도 근이 float 1.0이 될 수 있다)이 0·1이 아니면 비트가 같고, 유한한 끝의 차가 넘치는 경우만 Rocks가 유한한 값을 낸다(MathUtility.h 127–131).
3. ★ 가중 접선 3차 방정식에서 근이 2개일 때 초기화되지 않은 세 번째 칸을 읽지 않는다(CE 167).
4. ◇ NaN 시간은 NaN을 낸다(UE는 마지막 키 값). 빈 커브는 NaN이어도 기본값이다.
5. ◇ `PlainCurve`는 Cubic을 표현할 수 없다(`PlainCurveInterp`, UE는 ensure 후 저장값을 Linear로 평가).
6. ◇ `FindKey`는 허용오차 안의 키 중 가장 앞 키를 돌려준다(UE는 탐색 경로에 따라 정해진다).
7. ★ Bake 시간은 누적 덧셈 대신 `first + i × interval`(UE float 누적 오차, `first == last`면 `Reserve(−1)` fatal — RC 900).
8. ★ 범위판 `RemoveRedundantKeys`의 비교 기준을 시작 보존 키로 둔다(UE `MostRecentKeepKeyIndex = 0`, RC 1017).
9. ★ 계단 커브 `RemoveRedundantKeys`가 마지막 남은 키를 기본값으로 바꾸지 않는다(UE `StringCurve.cpp` 232–237·`IntegralCurve.cpp` 251–256). String은 첫 키 이전 평가 결과가 늘 바뀌고, Integer는 `useDefaultBeforeFirstKey`일 때 바뀐다.
10. ◇ 같음은 모든 칸 비교다(실수 비트·String byte·Name은 Name 규칙 — 계단 커브 중복 제거는 표시 철자까지, 17.5절 S5). UE는 가중치·비Cubic 접선·기본값을 빼고, String 키는 대소문자를 무시한다.
11. ★ 압축 키 시간 판정의 표본 구간 탐색(RC 1399–1426).
12. ◇ 압축 바이트는 리틀 엔디언 명시(UE 원시 메모리).
13. ★ 압축 Weighted 평가의 가중 판정 위치·순환 오프셋 누락, 풀기의 접선 손실·Empty 무한 반복·Constant 미처리(RC 2077–2105, 2118, 2441–2452).
14. ◇ 키 핸들은 값 밖의 표이고 표마다 결정적으로 발급한다(UE 전역 atomic). 표 대입이 옛 핸들을 남기지 않고(KH 74–76), 표를 거치지 않은 수정은 표 전체를 무효로 한다.
15. ◇ 숨김 열거자 `RCIM_None`·`RCTM_None`, 보이는 열거자 `RCCE_None`(동작은 Cycle — RC 1115·1139), `SmartAuto`가 없다.
16. ◇ 편집 연산이 접선을 몰래 다시 계산하지 않는다(6절 — `SetKeyTime`·`DeleteKey`·`SetKeys`·`RemoveRedundantKeys` 뒤 결과가 UE와 다르고, 호출자가 `AutoSetTangents`를 부르면 같다).
17. ◇ 기본값 "없음"은 Optional(UE `MAX_flt`·`MAX_int32`·빈 문자열). Name 계단 커브에 기본값이 있다(UE에 없다).
18. ◇ 값 범위는 해석적 극값(UE 표본 재귀).
19. ◇ 리플렉션 경로의 정렬·유한 시간·압축 정합 위반은 InvalidArgument다(UE는 확인하지 않는다).
20. ★ 압축 Constant 형식은 비트 같은 값만(UE 허용오차 1e-8 — RC 1286).
21. ★ 압축 Weighted 형식은 구간 단위 가중 판정(UE는 Cubic 키의 가중 방식만 — RC 1296–1302).
22. ★ `InterpCurve` 값 범위의 루프 닫는 구간에 루프 끝 시간을 쓰고, 키 하나의 루프 커브도 그 닫는 구간을 본다(UE는 첫 키 입력값을 끝 시간으로 넘기고 키 하나면 구간을 보지 않는다 — UE `InterpCurve.h` 746–772).
23. ◇ 허용오차 인자(UpdateOrAdd·중복 제거)가 NaN·음수면 InvalidArgument, `FindKey`는 `IndexNone`이다(UE는 음수를 그대로 써 늘 불일치·늘 보존 — 17.4절 C5).
24. ★ 회전 풀기는 UE가 끝나는 입력에서 비트가 같고, 360을 더하고 빼도 값이 그대로인 큰 값(UE는 끝나지 않는다)·비유한 값은 double 나머지로 옮기거나 그대로 둔다(17.4절 C6).
25. ★ Bake는 표본 2^24개 상한(CapacityExceeded), 표본판 중복 제거는 프레임 번호 int32 끝에서 멈추고 호출 하나 표본 2^24개 상한(CapacityExceeded)이다(UE는 감싸 돌아 끝나지 않거나 상한이 없다 — 17.4절 C9).
26. ★ 중복 제거가 차가 NaN인 키(NaN 값·접선)를 남긴다(UE `> tolerance` 비교는 지운다).
27. ◇ `TrySetKeys`는 정렬·유한 시간을 확인한다(UE `SetKeys`는 확인하지 않는다).
28. ◇ `TryGetTimeRange`·`TryGetValueRange`는 키가 없으면 NotFound다(UE는 (0, 0)).
29. ◇ `TryShiftTime`·`TryScaleTime`은 키 순서를 그대로 둔다(UE `ShiftCurve`·`ScaleCurve`는 키마다 `SetKeyTime`이라 같은 시간이 된 키의 순서가 바뀔 수 있다).
30. ★ 압축 키 시간 판정: 표본 시간은 누적하지 않고(`min + k·(1/rate)` — UE float 누적, RC 1435), errorThreshold·sampleRateHz가 NaN·Inf이거나 표본이 2^24개를 넘으면 Float32다(UE는 0 이하만 보고 표본 수 상한이 없다). 시간이 모두 같은 키(Δ = 0)도 Float32다(17.7절 P4).
31. ◇ 압축 풀기는 Linear 형식의 접선, Linear·Cubic·Mixed 형식의 가중 칸을 0·None으로 푼다(압축이 담지 않는다 — UE도 같다). 압축 칸은 정규형이다(Empty만 기본값, Constant만 상수 칸 — 17.7절 P5).
32. ★ 압축 키 시간 판정(UE `FindRichCurveKeyFormat`)의 손실 키 전진 뒤 범위 검사가 `CurrentLossyKey >= NumKeys`라, 복원한 마지막 키 시간이 max보다 작으면(1 ulp — 예: Linear 키 (−3, 0)·(6.4, 1), 1e-4·120 Hz) `Keys[NumKeys]`를 읽는다(RC 1413–1421 — 배열 밖 읽기, check 빌드는 assert). Rocks는 표본을 `Evaluate`와 같은 평가로 보므로 해당 없고, 시험 참조는 `CurrentLossyKey + 1 >= NumKeys`로 막아 옮겼다(17.7절 P10). 고친 `break` 갈래가 UE 판정을 바꾸는 반례(키 7개 이상)는 찾지 못했다: 건너뛰는 표본은 (복원한 마지막 키 시간, max]의 1 ulp 구간뿐이라 판정이 바뀌려면 누적 표본 시간이 그 구간(사실상 max)에 정확히 떨어지고, 마지막 구간 기울기 × 1 ulp가 threshold를 넘으면서 다른 키의 양자화 오차는 threshold 안이어야 한다. 그래서 고정 시험(`PackedTangentCurve_UeLossyKeyOverrunCounterexamples`)은 범위 밖 읽기 회귀 시험이다.
33. ◇ 압축 키 시간 양자화의 Clamp는 UE `FMath::Clamp`(Max(Min(X, 1), 0) — NaN이면 1)와 같은 선택이다(17.7절 P4). Rocks `math::Clamp`(NaN 그대로)를 쓰지 않는다.
