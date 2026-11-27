# 머티리얼 시스템(MaterialSystem) 설계

## 6. IR — MS2a 구현 세부

### 6.8 MS2a 구현 세부(MaterialCompiler 기반)

MS2a는 세 묶음으로 나눈다. 묶음마다 시험을 함께 넣고, 리뷰는 MS2a 끝에 한 번 받는다.

| 묶음 | 내용 |
|---|---|
| MS2a1 | 모듈 `Engine/Source/Developer/MaterialCompiler`(의존 MaterialSchema·ShaderCore·Core), IR 타입·연산·값 표, hash-consing 빌더, 상수 접기·대수 단순화, 진입점에서 닿는 값(DCE), IR hash |
| MS2a2 | 그래프 → IR 번역기: 입력 snapshot, lowering 표, 출력 노드 스칼라 핀, 상수·수학·`DynamicBranch`·`Operator`·재경유 lowering, V1·V2·V4·V5·V7·V8·V11·V12 진단 |
| MS2a3 | HLSL 생성(7.7절)·`MaterialCommon.rsh`·원문 위치 지도, MS-T1·MS-T2·MS-T3 |

**IR 타입.** `IrType = { kind, components, rows }`다.
- kind: `Float`·`Int`·`UInt`·`Bool`·`Double`(LWC 값, 5.3절)·`Matrix`(Float, rows×components)와 MS2c 뒤의 자원·`Surface`·`MaterialAttributes`.
- 수 타입의 components는 1–4, 행렬은 3×3·4×4만이다(5.3절).
- 핀 타입 `Numeric`·`Any`는 IR 타입이 아니다. lowering이 연결된 값의 타입으로 정한다.

**연산.** `IrOp`은 UE `EMaterialExpressionOperatorKind` 전부(Operator 노드 — 사칙·비교·논리·비트·shift·IsNan·IsInf·초월·쌍곡·Rsqrt·Transpose·행렬곱 등)와 다음을 담는다: `Constant`, `Splat`(스칼라 → 벡터 복제), `Swizzle`(성분 선택 — 즉시값에 성분 번호 넷), `Append`, `Convert`(수 종류 사이 변환), `Select`(조건 선택 — 둘 다 계산), `Branch`(`DynamicBranch` — 조건 분기 표지가 붙은 선택), `Extern`(문맥 값 표 번호, MS2b — 조건 값은 별도 연산 없이 Bool 문맥 값의 `Select`, 6.9절 MS2b3).

**값.** `IrValue = { 타입, 연산, 피연산자 값 번호 최대 MaxIrOperands(3)개, 즉시값, 빈도, 원인 노드 }`.
- 상수의 즉시값은 성분별 비트 패턴이다(float는 32 bit, double은 64 bit). 비교와 hash는 비트로 한다(−0과 0은 다른 상수, NaN은 V11로 막는다).
- 원인 노드(NodeId·PinKey)는 진단과 원문 위치 지도용이다. hash-consing 키에 넣지 않는다. 같은 값을 처음 만든 노드가 남는다.
- 빈도는 6.2절 격자다. MS2a의 값은 `Constant`·`Static`·`Pixel` 정도만 나온다.

**빌더.** 값을 만들 때 (타입, 연산, 피연산자, 즉시값, 진입점 문맥)으로 찾고 있으면 그 번호를 돌려준다(hash-consing, 6.5절 CSE). 표는 Core HashMap이고, 값 번호는 처음 만든 순서다. 순회 순서는 값 번호뿐이라 해시 표 순서에 기대지 않는다(6.6절).

**상수 접기(6.5절).** 피연산자가 모두 상수면 빌더가 바로 접는다. 접는 연산은 정확히 반올림되는 것뿐이다: 사칙·`min`·`max`·`abs`·`floor`·`ceil`·`round`(짝수 쪽)·`trunc`·`frac`·`saturate`·비교·논리·선택·swizzle·append·splat·정수 변환. `sqrt`는 IEEE가 정확한 반올림을 요구하지만 Core 결정적 수학에 함수가 없어 지금은 접지 않는다(호스트 libm을 쓰지 않는다 — 생기면 접기에 더한다). 초월 함수는 Core 결정적 수학 의존이 생길 때까지 접지 않는다(셰이더 시스템 10절 — 호스트 libm 금지). 접은 결과가 NaN·Inf면 접지 않고 남긴다(GPU와 같은 결과를 보장하지 못한다 — 0 나누기 등). 원문 쪽 상수 리터럴은 V11로 NaN·Inf가 없다.

**대수 단순화.** `x*1`·`1*x`·`x-0`·`x/1`·이중 swizzle 합치기·같은 성분 그대로의 swizzle 제거만 한다(6.5절). 정수 `x+0`은 IEEE 문제가 없으므로 한다. float `x+0`·`x*0`은 하지 않는다.

**진입점과 DCE.** 모듈은 진입점(7.3절 함수)마다 결과 값 번호를 가진다. 원문 생성과 IR hash는 진입점에서 닿는 값만 본다. 값 번호는 6.6절대로 진입점 순서 → 핀 순서 → 피연산자 순의 깊이 우선 후위 순으로 다시 매긴다(생성 순서와 다를 수 있다).

**IR hash.** 다시 매긴 번호 순으로 값마다 (타입, 연산, 피연산자 번호, 즉시값)을 고정 형식 little-endian으로 쓰고 진입점 표를 붙인 바이트의 SHA-256이다(Core Sha256).

**번역기(MS2a2).**
- 입력 `MaterialTranslationInput`: 그래프 문서(`GraphDocument`, 도메인 Material), 노드 카탈로그, NamePool, 기본 속성(`MaterialTraitsDesc` — 영역·블렌드 모드·반투명 속성), 진단용 자산 id. 객체 주소는 받지 않는다(6.3절 1항). 품질 수준·static 변형은 MS2b가 더한다.
- 결과 `MaterialTranslation`: IR 모듈, 진단 목록(NodeGraph `GraphDiagnosticList` — 14장 E4), 성공 여부. 오류 진단이 하나라도 있으면 성공이 아니고 모듈은 비어 있다. 번역 함수의 `Result` 실패는 할당·NamePool 같은 시스템 오류뿐이다(그래프 오류는 진단이다). UE도 오류를 모아 보고하고 번역을 실패로 끝낸다.
- 노드 속성은 `NodePropertyReader`로 읽는다(19.13.3절과 같은 경로 — MaterialCompiler는 ObjectSerialization에 의존하지 않는다). 핀 목록은 정의의 정적 핀 + 동적 핀 함수다(NodeTypeRegistry 없이 정의에서 직접 계산한다).
- **번역 순서.** 출력 노드의 활성 핀에서 시작해 간선(과 이름 있는 재경유의 선언)을 거슬러 닿는 노드를 반복 깊이 우선(명시 스택)으로 훑어 의존 후위 순서를 만든다. 훑는 중 방문 중인 노드를 다시 만나면 V2이고 그 입력은 끊는다(재경유 포함). 그 순서로 노드마다 lowering 함수를 한 번 부르고 (노드, 출력 핀)마다 결과를 기억한다. 그래서 함수가 요청하는 입력은 늘 이미 번역된 값이고 그래프 깊이가 C++ 스택을 쓰지 않는다(수천 단계 사슬도 된다). 닿는 노드의 연결된 입력은 모두 번역한다. UE 기본 번역기도 노드가 연결된 입력을 모두 컴파일하므로 쓰지 않는 쪽의 오류도 남는다(예: DynamicBranch의 고르지 않은 쪽). 비활성 노드(`NodeEnableState`)는 통과 쌍이 있으면 그 입력을 그대로 내고, 없으면 연결이 없는 것처럼 다룬다(NodeGraph 6.5절).
- **lowering 표.** 노드 종류 Name → lowering 함수다. 함수는 노드의 입력 값을 요청하고 출력 핀마다 IR 값을 낸다(UE IR `Build`처럼 마스크 출력도 함수가 직접 낸다 — 예: Constant2의 R 핀은 성분 0). 정적 출력 핀 타입은 연결 판정용이고(마스크 출력은 마스크 폭 — 19.13.3절), 실제 값의 타입은 lowering이 낸 값이다(`Numeric`·`Any` 출력). 표에 없는 종류가 진입점에 닿으면: 카탈로그에 없는 종류면 V12, 카탈로그에 있으나 이 단계에서 아직 lowering하지 않는 종류면 `NotYetLowered`(상태가 TODO면 문구에 선행 MPQ 이름을 넣는다). 노드의 단계 이름은 목록 파일(19.2.2절)에만 있으므로 문구에 넣지 않는다. MS2 전체가 끝나면 이 코드는 TODO 노드에만 남는다.
- **의미 기준.** 결과 값은 UE 기본 번역기(`FHLSLMaterialTranslator`, 6.1절)와 같다. 구조(출력마다 값, 입력 기본값 상수)는 UE IR `Build`(`RE/Private/Materials/MaterialExpressionsToMIR.cpp`)를 따른다. 둘이 다르면 기본 번역기를 따르고, 기본 번역기 쪽이 명백한 결함이면 IR `Build`를 따른다(아래 표에 적는다).

**핀 사용자 값.** NodeGraph `PinValueEntry.value`의 Record struct는 `MaterialPinValue { x, y, z, w : float; componentCount : uint8 }`이다(MaterialSchema 공개 리플렉션 struct, 5.3절). componentCount는 1–4이고 타입이 정해진 핀(`Float1`–`Float4`)이면 그 폭 이하다. 연결이 없으면 번역은 이 값을, 없으면 정의 기본값(`MaterialPinDefault`)을 같은 폭의 Float 상수로 낸다. 타입이 정해진 정수·Bool 핀이면 그 수 종류로 바꾼다. 값 편집을 숨긴 핀(`HideValue`)의 값은 남아 있어도 쓰지 않는다(필수면 `MissingInput`). 값이 유한하지 않으면 V11, 폭이 규칙 밖이면 `InvalidPinValue`다.

**입력 값과 변환.**
- 연결이 없는 입력: 핀 사용자 값 → 정의 기본값 → (없고 `Required`면) `MissingInput` 오류, (없고 필수가 아니면) 노드 규칙(UE `Compile`의 기본 처리 — 예: If의 AEqualsB는 없음).
- 출력 노드 핀(UE 속성 타입)과 lowering이 명시한 곳의 암묵 변환(5.3절). 노드 입력 핀 타입은 연결 판정용이라 번역이 값을 그 타입으로 바꾸지 않는다(UE `Compile`도 입력 값을 그대로 쓴다 — 예: UIntToFloat 입력은 핀이 UInt1이어도 폭을 유지한다): 스칼라 → 복제, 긴 벡터 → 앞 성분(`NarrowedValue` 정보 진단), 짧은 벡터 → 긴 벡터 V1, Int·UInt → Float 허용, Double → Float는 낮춤(UE LWC demote), Float → Int·UInt V1, Bool은 Bool 핀만, `StaticBool` → `Bool` 허용. `Numeric`·`Any` 핀은 값 그대로다.
- **오류 전파.** 연결된 입력이 값 없이 들어오면(위쪽 노드가 이미 오류를 진단했다) 그 노드도 실패로 표시하고 다시 진단하지 않는다. 오류 하나는 진단 하나다(UE `INDEX_NONE` 전파와 같다).
- **비활성 노드.** 통과 쌍이 있으면 그 입력을 넘기고, 없으면 연결이 없는 것과 같다. 비활성 노드의 위쪽(통과 쌍 입력 밖)은 번역 순서에 넣지 않는다. 연결 판정(If의 AEqualsB, 출력 핀의 V5·Pending)도 같은 규칙이다.
- **첫 인자 타입(UE `CoerceParameter`).** UE 기본 번역기가 첫 인자 타입을 결과로 쓰는 연산(Clamp의 Input, Fmod·Modulo의 A, Power의 Base)은 다른 입력을 그 타입으로 맞춘다: 수 종류를 바꾸고, 폭은 같으면 그대로, 스칼라는 복제, 대상이 스칼라면 첫 성분이다. 폭이 다른 두 벡터는 V1이다(UE `CastValue`는 스칼라가 아닌 대상으로 자르지 않는다 — `HLSLMaterialTranslator.cpp:4127–4131`).
- **산술 공통 타입**(두 피연산자 이상): 폭은 같거나 한쪽이 스칼라(넓은 쪽으로 복제)여야 하고 아니면 V1("Arithmetic between types … is undefined" — UE `GetArithmeticResultType`). 수 종류는 같으면 그대로, Int·UInt와 Float이면 Float, Double과 Float·Int·UInt면 Double(LWC 승격 — Double을 받는 연산만), Int와 UInt는 V1(HLSL 암묵 규칙에 맡기지 않는다), Bool은 산술 V1이다. UE 기본 번역기는 `UInt1 + Float1`을 UInt로 보지만 Rocks는 Float로 본다(5.3절 "정수 → float 허용"과 같은 방향).
- **연산 묶음별 수 종류**(UE IR 연산 서명 `RE/Private/Materials/MaterialIREmitter.cpp:2303–2420`): (a) Double 허용 — Add·Subtract·Multiply·Divide·Min·Max·Abs·Floor·Ceil·Round·Truncate·Negate(단항은 Negate 말고 Int·UInt를 Float로 — UE `OPF_CastToAnyFloat`). (b) Float 전용(Double은 낮추고 Int·UInt는 Float로) — 삼각·역삼각·쌍곡·Exp·Log·Sqrt·Rsqrt·Reciprocal·Pow·Fmod·Step·Smoothstep·Lerp·Frac·Saturate·Sign·Dot·Cross·Distance·Length·Normalize·ATan2. (c) 정수 전용 — 비트 연산·shift(Float은 V1). (d) Bool 전용 — And·Or·Not. (e) 비교 — 산술 공통 타입, 결과 Bool(같은 폭). Modulo·Clamp는 Double을 받지 않는다(낮춘다 — UE도 Clamp는 min·max가 Double이 아니면 첫 인자를 낮춘다).

**lowering 표(MS2a 노드 62개).** 입력 이름은 Rocks 핀 키, "기본"은 정의 기본값이 있는 입력이다. 근거는 UE `MaterialExpressions.cpp`의 `Compile`과 `MaterialExpressionsToMIR.cpp`의 `Build`, 연산 원문은 `HLSLMaterialTranslator.cpp`다.

| 노드 | lowering | UE 근거·비고 |
|---|---|---|
| Constant·Constant2·Constant3·Constant4 | 속성 성분의 Float 상수. Constant2·3·4의 성분 출력(R·G·B·A)은 성분 하나, Constant4 RGB는 앞 셋 | `Compile` `Constant*`, `Build` `Subscript`(성분 출력). UE 출력 마스크 |
| ConstantDouble | Double 스칼라 상수 | `Build`만 있다(`GenericConstant`) |
| Abs·Floor·Ceil·Round·Truncate | 묶음 (a) 단항 | `Compiler->Abs` 등 |
| Frac·Saturate·Sign·SquareRoot·Exponential·Exponential2·Logarithm·Logarithm2·Logarithm10·Arcsine·Arccosine·Arctangent·ArcsineFast·ArccosineFast·ArctangentFast | 묶음 (b) 단항(Fast는 `ASinFast` 등 — 원문 `asinFast`) | 기본 번역기 원문 `frac`·`log`·`asinFast` 등 |
| Sine·Cosine·Tangent | `period > 0`이면 입력 × (2π / period)를 먼저 곱한다. 아니면 입력 그대로 | 기본 번역기의 Cosine·Tangent는 period ≤ 0에서 코드 번호 0을 곱하는 결함이 있어(`MaterialExpressions.cpp:5740, 5758`) IR `Build`(`GetTrigonometricInputWithPeriod`)를 따른다 |
| Add·Subtract·Multiply·Divide·Min·Max | A·B(기본) 묶음 (a) 이항 | |
| Clamp | `clampMode`: Clamp면 결과는 Input 타입이고 Min·Max를 그 타입으로 맞춘 Clamp(Input, Min, Max). Min·Max가 상수 0·1이면 Saturate(Input). ClampMin이면 Max(Input, Min), ClampMax면 Min(Input, Max)(산술 공통 타입). Min·Max 기본 0·1 | `HLSLMaterialTranslator.cpp:10572`. 원문은 `min(max(x, lo), hi)`(7.7절) |
| OneMinus | 1 − Input(1은 Input의 수 종류 — Double이면 Double 1) | `Build` `ConstantOne`. 기본 번역기는 float 1과의 뺄셈이라 Double 입력 결과도 Double이다 — 같다 |
| Fmod | 결과는 A 타입(Float로 — 묶음 (b))이고 B를 그 타입으로 맞춘 Fmod(A, B) | `:5795`. 원문 `fmod` |
| Modulo | 결과는 A 타입(Double 낮춤)이고 B를 그 타입으로 맞춘 Modulo(A, B) | `:5824`. 원문 `%` |
| Step | Step(Y, X) = X ≥ Y ? 1 : 0. Y·X 기본 0·1 | `step(Y, X)` |
| SmoothStep | Smoothstep(Min, Max, Value), 기본 0·1·0 | `smoothstep` |
| Power | 결과는 Base 타입이고 지수는 float 스칼라로 맞춘다(벡터면 첫 성분 — `CoerceParameter(Exponent, MCT_Float)`). 상수 지수면 두 타입이 산술로 맞는지 보고, 지수 1이면 Base, 0이면 1, 상수 밑 0이면 0이다. 그 밖은 Base ≤ 2.980233e−8이면 0, 아니면 pow(Base, 지수). Exponent 기본 2 | `HLSLMaterialTranslator.cpp:9819`, `PositiveClampedPow`(`SH/Private/Common.ush:811`) |
| LinearInterpolate | A와 B가 같은 값이면 A, Float 상수 Alpha가 0이면 A, 1이면 B(그대로 — lerp 식 반올림 없이). 그 밖은 Lerp(A, B, Alpha)(산술 공통 타입). 기본 0·1·0.5 | `:10367`. Alpha 폭이 결과와 다르고 스칼라도 아니면 V1이다(UE는 HLSL 암묵 변환에 맡긴다 — 편차) |
| InverseLinearInterpolate | (Value − A) / (B − A). `clampResult`면 Clamp(…, 0, 1). Value가 A와 같은 값이면 0, B와 같으면 1(둘 다면 0 — UE가 B 판정 뒤 A 판정으로 덮는다. 같은 식 판정은 같은 IR 값 번호로 본다). A와 B가 같은 값이면 `DivideByZero` 오류 | `HLSLMaterialTranslator.cpp:10264` |
| DotProduct | 한쪽이 스칼라면 넓은 쪽으로 복제한다. 폭이 다른 두 벡터는 V1이다(UE 주석은 "truncate"지만 `CoerceParameter`가 자르지 않아 오류다). 결과 Float 스칼라 | `Dot` 원문. UE는 한쪽이 상수 0이면 0을 내지만 Rocks는 IEEE 결과를 바꾸는 단순화를 하지 않는다(6.5절) |
| CrossProduct | 두 입력을 Float3로 맞춘 Cross | |
| Normalize | Normalize(VectorInput) — 스칼라도 `normalize` 그대로(부호) | 기본 번역기. IR `Build`의 스칼라 상수 1과 다르다(음수에서) |
| Length | 스칼라면 Abs, 벡터면 Length | 기본 번역기 최적화와 같다 |
| Distance | Length(A − B)(스칼라면 Abs) | `CompileHelperLength` |
| Append | A 성분 뒤에 B 성분(합 ≤ 4, 넘으면 V1). 한쪽이 Double이면 결과 Double | `Build` `AppendVector` |
| ComponentMask | `r`·`g`·`b`·`a`가 켠 성분을 순서대로. 스칼라 입력은 켠 성분마다 그 스칼라다(UE는 폭 미정 스칼라 `MCT_Float`에 g·b·a를 `.r`로 복제한다 — `:10776–10817`. Rocks IR 스칼라는 모두 그렇게 본다). 벡터 입력 폭보다 큰 성분을 켜면 V1, 하나도 안 켜면 V1 | 원문 swizzle |
| If | A·B(기본 0) 비교. AEqualsB가 있으면 Select(abs(A − B) > threshold, Select(A ≥ B, Greater, Less), Equals), 없으면 Select(A ≥ B, Greater, Less). 결과는 세 값의 산술 공통 타입. A·B가 벡터면 성분별(Select 조건이 같은 폭). 조건 폭과 결과 폭이 다르고 조건이 스칼라도 아니면 V1이다(UE는 HLSL 암묵 자름에 맡긴다 — 편차) | `HLSLMaterialTranslator.cpp:6575`. Greater·Less 셋이 같은 값이면 그 값 |
| DynamicBranch | Branch(Condition, True, False). Condition은 스칼라이고 수는 Bool로 바꾼다(StaticBool 허용 — UE `CastToBool`). 결과는 두 값의 공통 타입이고, 상수 조건이면 공통 타입으로 맞춘 고른 값을 낸다(UE도 ResultType으로 ForceCast한 뒤 고른다 — `HLSLMaterialTranslator.cpp:6473–6503`. 두 입력은 위 번역 순서대로 모두 번역한다). UE 기본 번역기는 float 결과를 `lerp(else, then, cond)`로 내지만(`:6515–6520`, 고르지 않은 쪽의 NaN·Inf가 번지고 반올림이 생긴다) Rocks는 진짜 분기다(편차 — 7.7절) | `Compile`(`MaterialExpressions.cpp:21855`)·`Build` `IfThenElse`, 원문 7.7절 |
| Switch | SwitchValue가 Float 스칼라 상수 v면 i ≤ v < i + 1인 입력 i, 없으면 Default 값을 낸다(입력은 모두 번역한다 — UE도 모두 컴파일한 뒤 고른다). 아니면 UE 식 그대로: Σ step(i, floor(s)+0.5)·step(floor(s)+0.5, i+1)·Inputᵢ + (step(floor(s)+0.5, 0) + step(n, floor(s)+0.5))·Default. 입력 i는 모두 필수(UE "missing input"), 결과는 입력들의 산술 공통 타입 | `HLSLMaterialTranslator.cpp:8437` |
| FloatToUInt | `mode`(Truncate·Floor·Round·Ceil) 뒤 UInt로 변환(폭 유지) | `ForceCast` |
| UIntToFloat | Float로 변환(폭 유지) | |
| ConstantBiasScale | (Input + bias) × scale | |
| Convert | 출력 i의 성분 j는 `convertMappings`에서 (출력 i, 성분 j)인 대응의 입력 성분, 없으면 출력 기본값. 입력이 연결되지 않으면 그 입력의 기본값(Float4). 성분을 Append로 잇는다(한 성분이라도 Double이면 Double — UE AppendVector의 LWC). 동적 핀이 속성과 맞지 않으면 `InvalidProperty` | `MaterialExpressions.cpp:22713` |
| Operator | `operatorKind`의 IrOp. Add·Multiply는 입력 전부를 왼쪽부터 접는다(UE `VariableInputsExpressionOperators`), 그 밖은 연산 인자 수만큼 앞 입력을 쓴다. 연결이 없는 입력은 그 원소의 `constValue` 스칼라. 입력이 인자 수보다 적으면 `InvalidProperty` | `MaterialExpressionsToMIR.cpp:3474`(Operator는 IR 경로에만 있다) |
| NamedRerouteDeclaration | Input 그대로 | `Compile`이 입력을 넘긴다 |
| NamedRerouteUsage | `declaration` id인 선언 노드의 Input. 선언이 없으면 `InvalidProperty`("Invalid named reroute variable") | 선언과 쓰임 사이 순환은 V2 |

**출력 노드 MS2a 핀과 진입점.** 활성 조건은 UE Substrate 경로의 `IsPropertyActive_Internal`(`RE/Private/Materials/Material.cpp`)이다. 비활성 핀에 연결이 있으면 V5 경고로 남기고 IR에서 버린다.

| 핀 | 진입점·slot | 활성(영역 / 블렌드 모드) |
|---|---|---|
| WorldPositionOffset | Vertex `WorldPositionOffset` | Surface·DeferredDecal·UserInterface |
| Opacity | Opacity `Opacity` | AlphaComposite(Surface·DeferredDecal·PostProcess·UserInterface — Substrate opacity override) |
| OpacityMask | OpacityMask `OpacityMask` | Masked(Surface·UserInterface) |
| PixelDepthOffset | PixelDepthOffset `PixelDepthOffset` | Surface, 반투명이 아니거나 반투명 velocity를 쓸 때(`outputTranslucentVelocity`) |
| AmbientOcclusion | Surface `AmbientOcclusion` | Surface·DeferredDecal. UE는 Surface에서 lit shading model일 때만이고 shading model은 MS2d가 Surface 입력에서 구하므로 그때 좁힌다 |
| Refraction | Surface `Refraction` | Surface, 반투명 ∧ AlphaHoldout·Modulate 아님 ∧ `usesDistortion` ∧ `refractionMethod` ≠ IndexOfRefractionFromF0(SingleLayerWater 조건은 MS2d) |
| SurfaceThickness | Surface `SurfaceThickness` | Surface ∧ `thinSurface` |

Surface·Attributes·CustomizedUv0–7·Displacement 핀은 MS2a에서 번역하지 않는다: 연결이 있으면 `NotYetLowered`다(Surface·Attributes·Displacement는 MS2d, CustomizedUv는 MS2b). 연결이 없는 핀은 진입점에 기본값을 낸다(7.3절 "쓰지 않는 함수는 기본값").

**진단 코드.** 코드는 V 번호(1–15)와 아래 번역 코드다. 문구는 영어다.

| 코드 | 이름 | 뜻 |
|---|---|---|
| 101 | MissingInput | 필수 입력에 연결이 없다(UE "Missing … input") |
| 102 | NotYetLowered | 카탈로그 노드지만 이 단계에서 번역하지 않는다 |
| 103 | InvalidProperty | 속성을 읽을 수 없거나 값이 규칙 밖이다(모르는 enum, 잘못된 대응 색인, 없는 선언) |
| 104 | NarrowedValue | 긴 벡터를 짧은 입력에 넣어 앞 성분만 썼다(정보) |
| 105 | InvalidPinValue | 핀 사용자 값 Record가 규칙 밖이다 |
| 106 | DivideByZero | 번역 때 0 나누기가 확정된다(InverseLinearInterpolate의 A = B) |
| 107 | GraphTooDeep | 번역 순서 계획 밖의 요청(의존을 미리 알 수 없는 노드)이 재귀로 번역될 때 그 깊이가 상한(`MaxLoweringDepth` 256)을 넘는다. 계획된 번역은 재귀하지 않으므로 지금 노드에서는 나지 않는 방어 상한이다 |

- **상수 접기 편차.** Round 상수는 짝수 쪽으로 접는다(GPU `round`와 같다). UE preshader는 0.5에서 올리는 `RoundToFloat`(`ShaderValue.cpp:971`)라 uniform 식에서만 다를 수 있다.
- **원문 위치.** 같은 값을 여러 노드가 만들면 원인 위치는 NodeId가 가장 작은 노드다(빌더가 다시 만날 때 고친다). 그래서 위치 지도도 노드·간선 저장 순서와 무관하다. 원인 위치는 hash-consing 키·IR hash에 들지 않는다.
- MS2a 진단: V1(타입·폭), V2(순환), V4(정점 진입점이 픽셀 전용 값 — MS2a 노드에는 없고 미분 노드가 MS2b에 온다. 검사는 IR 연산 `Ddx`·`Ddy`로 한다), V5(비활성 출력 핀 연결 — 경고), V7(StaticOnly 입력에 빈도가 Static보다 높은 값 — MS2a 노드에는 StaticOnly 입력이 없어 시험은 static 노드가 오는 MS2b에서 한다), V8(같은 이름·다른 종류 파라미터 노드 — 파라미터 표지 노드의 `parameterName`을 대소문자 무시로 본다. MS2c 전에는 노드 속성만 본다), V11(NaN·Inf 상수 — 노드 속성·핀 값), V12(모르는 노드).
