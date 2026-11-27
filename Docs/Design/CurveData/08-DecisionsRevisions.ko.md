# 커브 데이터 — 결정·미확인·변경 이력

[목차](README.ko.md)

## 18. 결정 목록

권장안으로 확정했다(2026-11-18, 교차검증 1차 반영).

- **D1 시간·값 정밀도** — float(UE와 같음). double 커브가 필요해지면 템플릿이 아니라 구체 타입(`TangentCurveDouble` 같은 별도 `ROCKS_STRUCT`)을 따로 둔다 — 리플렉션 struct는 템플릿일 수 없다(RMC2000).
- **D2 다형성** — virtual 기반 없이 상속 + 자유 함수. 공통 칸은 리플렉션 base `ScalarCurveBase`다. 다형 저장(`InstancedStruct<ScalarCurveBase>`)을 쓸지는 에셋 층이 정한다. 쓰면 평가 분기는 담긴 타입으로 고르는 자유 함수이고, 모르는 파생 타입·base 자체는 Unsupported, 비어 있으면 NotFound다(14절).
- **D3 키 핸들** — 값 밖의 편집 표(`CurveKeyHandleTable`), 표마다 1부터 단조 uint32·0 무효·Overflow 거부, 저장 안 함, 복사·대입은 핸들 그대로. 표는 커브를 쥐지 않고 함수마다 인자로 받는다. 표는 마지막으로 맞춘 키 배열의 사본을 쥐고 호출마다 비트 비교해, 표를 거치지 않은 수정이면 표 전체를 무효(StaleHandle)로 하고 `TryRebuild`로 다시 발급한다.
- **D4 UE 없음 열거자** — `RCIM_None`·`RCTM_None`(숨김)과 `RCCE_None`(보이는 열거자, 동작은 Cycle)을 두지 않는다(번호 3·3·5는 비워 둔다).
- **D5 SmartAuto** — 두지 않는다(UE `FRichCurve`가 계산하지 않는 MovieScene 전용 모드). 시퀀서 채널을 만들 때 그쪽에서 연다.
- **D6 "없음" 기본값** — `Optional<T>`(UE sentinel 대신). 압축 Empty 형식의 기본값도 Optional이다.
- **D7 불변식과 struct 검증 hook** — 채택. hook 범위는 정렬·유한 키 시간·압축 정합이다. `PlainCurve` 보간은 별도 enum `PlainCurveInterp {Linear, Constant}`로 Cubic을 표현할 수 없게 하고, 키 배열은 private PROPERTY + 접근자로 C++ 쪽 불변식을 API가 지킨다. hook 계약(noexcept·무할당·순수·아무 스레드·게시 전 1회)과 중첩 위치 규칙은 9.1절. CV3, 규모 L.
- **D8 wire 크기** — 키당 약 350 byte(UE 27 byte의 약 13배)로 정정했고 CV5에서 실측했다(`TangentCurveKey` 352 byte·`PlainCurveKey` 116 byte — 9.2절). 저작 데이터는 리플렉션 RecordList wire를 유지하고 런타임 데이터는 `PackedTangentCurve`로 확정했다. 열 단위 배열·전용 codec은 에셋 층에서 실제 크기가 문제로 측정될 때 다시 본다.
- **D9 Name 계단 커브** — UE `FNameCurve`에는 평가 함수도 기본값 칸도 없다. Rocks는 `Optional<Name>` 기본값을 더하고 String과 같은 규칙(첫 키 이전은 기본값)으로 평가한다.
- **D10 접선 재계산 시점** — 편집 연산은 몰래 다시 계산하지 않고 `AutoSetTangents`를 명시적으로 부른다. UE가 재계산하는 연산(`DeleteKey`·`SetKeys`·`SetKeyTime` 안의 삭제·`RemoveRedundantKeys`)과는 결과가 다르고, 호출자가 같은 지점에서 `AutoSetTangents`를 부르면 같다.
- **D11 값 범위** — Cubic 구간 극값을 해석적으로 구한다. 가중 구간은 Evaluate가 실제로 쓰는 매개변수 집합(x가 단조면 [0, 1], 아니면 7.3절의 S — 증가 조각 중 오른쪽 최솟값보다 작은 부분) 위에서 구하고, 뛰는 점은 두 쪽 극한을 모두 넣는다. 3차 방정식 풀이의 판별식 띠(|D| ≤ 1e-8)에서 풀이가 실제로 고르는 매개변수도 넣어 Evaluate가 내는 값을 모두 담는다(17.4절 C10).
- **D12 ReadjustTimeRange** — 값 타입에 두지 않는다(시퀀서·도구 층의 시간 범위 맞춤 기능).
- **D13 압축 바이트 순서** — 리틀 엔디언 명시, union 없이 칸을 나눈다(`constantValue`·`emptyDefault`).
- **D14 압축 평가 식과 형식 결정** — 원본과 같은 핵심 식(`EvalCurveSegment`)을 쓴다. 형식 결정은 구간 단위 가중 판정으로 고치고, Constant는 비트 같은 값만이다. 그래서 Float32 압축이 원본과 비트가 같다(UE 압축 평가의 곱셈 순서·허용오차를 따르지 않는다).
- **D15 모듈** — 수학 핵심은 Core(`KeyCurveMath.h`, 접선 시간 차는 `InterpCurveMath.h` 재사용), 프레임 시간은 Core `Time`, 값 타입은 새 Core 런타임 모듈 `Curves`, InterpCurve 미러는 `CoreTypeBindings`, Engine 층에는 두지 않는다.
- **D16 순환 cycle 수 범위 밖** — |float 몫| < 2^30이면 double로 넓혀 `TryFloorToInt32`로 계산하고(UE와 같은 식), 그 밖이면 double floor·나머지(fmod 계열)로 재배치해 순환 의미(정확한 배수에서 max로 가는 규칙 포함)를 지킨다. Fatal로 가는 경로가 없다(5.1절).
- **D17 비트 기준** — Windows x64에서 UE와 비트가 같은 것을 목표로 한다. UE `SinCos(float)` 다항식은 그대로 옮긴다(플랫폼과 무관하게 결정적). `atanf`와 double libm(`acos`·`cos`·`sqrt`·`pow`)은 UE와 같은 함수를 쓰고, 그 부분의 플랫폼 간 비트 결정성은 보장하지 않는다(UE와 같은 수준). DeterministicMath 함수로 바꾸지 않는다. CV1에서 확인한 조건(17.1절 K12): 비교 대상은 UE `/fp:precise` 빌드(Editor·Program 타깃 — 게임 타깃 기본은 `/fp:fast`라 식 자체의 비트가 정해지지 않는다)이고, 두 쪽 모두 릴리스 C 런타임(`ucrtbase.dll`, `/MD`)일 때다.
- **D18 바이트 덩어리 리플렉션 값** — 기반에 보강해 채택한다. `Array<uint8>` 필드·멤버·원소의 wire를 ValueArray(원소당 U32 4 byte)에서 Bytes 블록 하나(u64 길이 + byte)로 바꾼다(하위호환 없음 — 지금 쓰는 곳은 시험 fixture뿐). 경로 원소 접근과 JSON 표기 등 세부는 ReflectionExtensions 개정에서 정한다. `PackedTangentCurve::packed`가 쓴다. CV4.
- **D19 InterpCurve 미러·값 범위** — 이 Curve 단계에 넣는다. 미러 대상은 UE USTRUCT가 있는 다섯 형식이다. private `points_`·`looped_`·`loopKeyOffset_`는 Core 타입의 `GeneratedType` friend 선언과 MetaCompiler의 Binds private 멤버 접근 보강으로 노출한다(접근자 미러가 아니다). 도구 이름은 `Name=`으로 둔다. 미러에 `Validate`를 둔다. `TryCalcBounds`는 UE `FindBounds` 식을 옮기고 루프 닫는 구간만 고친다. CV9.
- **D20 FrameRate·FrameTime** — Core `Time`에 최소 타입 `FrameRate`(분자·분모 int32)·`FrameNumber`(int32)·`FrameTime`(프레임 + subframe float)을 두고 UE `AsFrameTime`·`AsSeconds` 식을 옮긴다. `RemoveRedundantKeys` 표본판이 `Optional<FrameRate>`로 쓴다. 제거 뒤 `AutoSetTangents`를 부르지 않는 D10의 결과 차이는 7.2절에 적었다. CV2.
- **D21 압축 정합** — 평가 진입에서 O(1) 크기 대조(enum 범위, keyCount ≥ 2, `packed.Size() == ExpectedPackedSize(…)`)를 하고, 불일치면 `Evaluate`는 quiet NaN, `TryEvaluate`는 InvalidArgument다. 검증 hook은 같은 정합과 byte 값·시간 정렬까지 보고 decode·대입에서 거부한다.

## 19. 확인하지 못한 것

- UE `RCTM_SmartAuto`의 세부 식(시퀀서 CVar별 분기) — D5로 범위 밖이라 확인하지 않았다.
- UE `FRichCurveKey`의 실제 메모리 크기(28바이트로 보이지만 static_assert로 확인하지 않았다) — Rocks 설계에 영향 없음.
- ~~가중 경로의 `atanf`·double libm이 Rocks Windows x64 빌드(Clang + MSVC 런타임)와 UE 빌드(MSVC)에서 같은 C 런타임 구현으로 묶이는지~~ — CV1에서 확인했다(17.1절 K12): 같다. D17의 Windows x64 비트 목표를 유지한다(릴리스 C 런타임 조건).
- ~~리플렉션 키 wire의 정확한 byte 수(기본값 필드 생략 여부)~~ — CV5에서 실측했다(9.2절): 생략 없이 키당 352 byte.
- ~~Core enum(`InterpCurveMode`)의 Binds 미러 지원 여부, Binds 생성 코드가 실제 타입의 private 멤버에 `GeneratedType` friend로 닿는 경로~~ — CV9에서 확인했다(17.8절 I2·I3): enum Binds는 없어 더했고, private 멤버는 friend로 생성 코드 수정 없이 닿는다.
- UE 압축 Weighted 결함(11절)이 UE 쿡 데이터에서 실제로 드러나는지(애니메이션 코덱이 Weighted를 고르는 빈도) — Rocks 설계는 결함을 고치는 쪽이라 확인하지 않아도 된다.
- ~~가중 구간 값 범위(7.3절)의 비단조 판정이 x′ 근의 수치 오차로 갈리는 경계~~ — CV5 단계 리뷰에서 실제 문제는 x′ 근이 아니라 Evaluate 풀이의 판별식 띠(|D| ≤ 1e-8)였다. 범위가 그 띠에서 풀이가 고르는 매개변수를 넣도록 고쳤고, 띠를 겨냥한 무작위 표본(접힘 점 근처 포함)으로 확인했다(17.4절 C10).

## 20. 변경 이력

- **개정 1 초안(2026-11-18, 36027f9)**: 값 타입·평가·편집·핸들·압축 설계, 결정 D1–D15, 단계 CV1–CV7.
- **개정 13(2026-11-18, CV10 묶음 끝 리뷰 반영)**: 빈 루프 커브를 받는 미러 hook(12.1절·I4), enum Binds 열거자 1:1 AST 대조 RMC2401(I2), 닫는 구간 길이 = offset(12.2절·I5), 설정 배열 연산 정규화는 hook 없이(I7), UE 범위 함수 형식 명시(I6), 15절 32 반례 탐색 결과, 진행 표 CV10 완료.
- **개정 12(2026-11-18, CV10)**: 17절 진행 표에 상태 칸(CV1–CV10 완료·커밋), 17.9절(다른 문서 반영 목록·남은 일·두지 않은 UE 기능·UE 결함 기록·한계). 다른 문서: 에셋 시스템 Curve 행, Core 상세 G09, InterpCurve 구현 문서, 리플렉션 보강 23–25절, 리플렉션·빌드 보강 4.5절.
- **개정 11(2026-11-18, CV9 구현·CV8 단계 리뷰 반영)**: 17.8절 I1–I6(InterpCurve 미러 다섯 형식·enum Binds·private 멤버 friend·미러 Validate·`TryCalcBounds`), 19절 확인 결과. CV8 리뷰 — 15절 32·33(UE 손실 키 범위 밖 읽기, UE Clamp), 17.7절 P3 예시·P4(비유한 역수·간격·복원 시간은 Float32, 시간 전용 탐색)·P9(이동된 쪽 Empty)·P10, 15절 30 표본 식, 17.5절 S5(Name 토큰 모든 칸).
- **개정 10(2026-11-18, CV8 구현·CV6·CV7 묶음 리뷰 반영)**: 리뷰 — 17.6절 H4(`TryEditKeys` 키별 칸 편집자·`TryAutoSetTangents`·표 판 중복 제거·Shift·Scale, UE `ShiftCurve` 핸들 차이)·H5 예외·H8·H9·H10, 17.5절 S5(Name 중복 제거는 표시 철자까지)·S7, 7.3절 반례 값 정정. CV8 — 17.7절 P1–P10(압축 커브 — Constant 외삽·유한 조건, 키 시간 판정을 Evaluate와 같은 평가로, 칸 정규형, 한도, 평가·풀기), 11.1·11.2·11.4절 정정(Bytes wire는 CV4에서 적용됨), 15절 30–31.
- **개정 9(2026-11-18, CV7 구현 반영)**: 17.6절 H1–H8(키 핸들 표 — 칸별 비트 비교 감지, 표 판 편집 함수, 원자성, `TryEditKeys`).
- **개정 8(2026-11-18, CV6 구현·CV5 단계 리뷰 반영)**: 17.5절 S1–S7(계단 커브 셋 — NaN 시간은 기본값, String 빈 기본값, 중복 제거의 같음 규칙, `FindKeyAtOrBefore`). CV5 리뷰: 값 범위가 풀이 판별식 띠의 매개변수까지 담음(7.3절·C10·D11·19절), 회전 풀기 UE 비트 일치(C6), 표본판 상한·NaN 키 보존(C9), 순환 int32 경로 벗어남 표현(5.1절·C4), golden 참조 표현(C13), 15절 23–29, D8 확정, 7.1표.
- **개정 7(2026-11-18, CV5 구현·CV4 단계 리뷰 반영)**: 17.3절 B8·B5, 17.4절 C1–C14(`Curves` 모듈, 값 범위의 일반 매개변수 집합, API 이름, 허용오차·회전 풀기·Bake·중복 제거 경계), 9.2절 키 wire 실측과 저작 wire 유지 판단, D8·19절 갱신.
- **개정 6(2026-11-18, CV3 단계 리뷰 반영)**: 17.2절 V9–V12.
- **개정 5(2026-11-18, CV4 구현 반영)**: 17.3절 B1–B7(`Array<uint8>` Bytes 블록 wire, Base64 텍스트).
- **개정 4(2026-11-18, CV1·CV2 단계 리뷰·CV3 구현 반영)**: 순환 int32 경로를 |몫| < 2^30으로 좁히고 double 경로의 끝 바로잡기를 뺌(5.1절, 15절 1, D16, 17.1절 K3·K8), UE 비트 범위 기준을 Bezier에 넣는 비율로(15절 2, 16절, K13), 17.2절 V1–V8(struct 검증 hook).
- **개정 3(2026-11-18, CV1·CV2 구현 반영)**: 17.1절 K1–K12. 5.4절 double 시간 혼용 설명 정정(float와 늘 같음)과 근 고르기 문구, 16절 해당 시험, D17 조건(UE `/fp:precise`·릴리스 C 런타임), 19절 1번 확인 결과.
- **개정 2(2026-11-18, 교차검증 1차 반영, 결정 D1–D21 확정)**:
  - 사실 정정: UE `SinCos(float)`는 libm이 아니라 다항식(5.4절, D17). 가중 경로의 키 시간 double 혼용을 의사코드에 반영(5.4절). `SolveCubic` 연산 순서를 UE 식 그대로 적음. Windows x64 UE `FloorToInt`는 SSE 식(5.1절). `RCCE_None`은 숨김이 아님(2절, D4). UE `SetKeyTime`은 안의 `DeleteKey`로 접선을 재계산함(6절). `PlainCurve`·`TangentCurve`의 첫 키 시간 중복 차이(5.7절), 마지막 키 시간 중복 결과(5.2절). `Lerp` 끝의 차 넘침 예외(5.3절, 15절 2). 계단 커브 중복 제거의 Integer 조건(15절 9). UE `FNameCurve`에 기본값이 없음(4절, D9). wire 크기 약 13배(9.2절, D8). 압축 보간 byte는 형식 값(11.3절). 압축 "앞 경계 비교" 차이 주장은 결과 차이가 없어 지움(11.5절).
  - 치명 반영: cycle 몫 넘침이 Rocks `FloorToInt32` Fatal로 가는 경로를 D16으로 막음(5.1절). 압축 칸 정합 미검증의 메모리 밖 읽기를 D21(평가 진입 O(1) 대조)과 hook으로 막음(11.4절).
  - 설계 보강: struct 검증 hook 계약·중첩 위치(9.1절, D7), private 키 배열과 `PlainCurveInterp`(4절), 키 핸들 표의 연결·감지 방식(8절, D3), 압축 형식 결정의 구간 단위 가중·비트 같음 Constant(11.1절, D14), Empty 기본값 Optional(11.4절), 값 범위의 가중 비단조 처리(7.3절, D11), `Array<uint8>` Bytes 블록 wire(D18), InterpCurve 미러와 `TryCalcBounds`(12절, D19), `FrameRate`·`FrameTime`과 중복 제거의 D10 관계(7.2절, D20), 선택 키 이동(7.1절), 자동 접선 시간 차 하한 재사용(3·6절), D1의 구체 타입, D2의 에셋 층 위임과 모르는 파생 타입 규칙(14절).
  - 시험·단계: 독립 고정 벡터, 참조 코드 원본 대조 리뷰, SSE `FloorToInt` 참조, 넘침·변조 packed·hook·Bytes·FrameRate·InterpCurve 미러 시험 추가(16절). 단계를 CV1–CV10으로 늘리고(FrameRate·hook·Bytes·packed 정합·InterpCurve 미러) 교차검증을 단계마다 둠(17절).
