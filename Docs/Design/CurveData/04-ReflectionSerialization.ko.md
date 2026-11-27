# 커브 데이터 — 리플렉션·직렬화·압축 커브

[목차](README.ko.md)

## 9. 리플렉션·wire·JSON

| B9 기능 | 쓰는 곳 |
|---|---|
| struct 상속(B9k) | `TangentCurve`·`PlainCurve`가 `ScalarCurveBase`를 상속(기본값·외삽 필드 ID는 base 소유) |
| Optional(B9e) | `defaultValue`(UE의 `MAX_flt`·`MAX_int32`·빈 문자열 "없음" 표지 대신), `PackedTangentCurve`의 Empty 기본값 |
| Name(B9c) | `NameStepKey::value`·`NameStepCurve::defaultValue` |
| enum underlying(B9a2) | 커브 enum은 `uint8` underlying(UE `TEnumAsByte`와 같은 1바이트 — wire는 I64) |
| Atomic(B9b) | 키 struct |
| private 필드·필드 `Name`(B2) | `keys_`(`Name = "keys"`) |
| 이 문서가 보강하는 것 | struct 검증 hook(9.1절, D7), `Array<uint8>`의 Bytes 블록 wire(11절, D18), 미러의 private 멤버 접근(12절, D19) |
| InstancedStruct·Map·Text·bitfield·WeakObjectRef·ClassRef | 이 층에서는 쓰지 않는다(InstancedStruct·Map은 에셋 층이 고를 수 있다 — 14절) |

- JSON은 ReflectionText 규칙 그대로다. 예:

```json
{"defaultValue": null, "preExtrapolation": "Constant", "postExtrapolation": "Cycle",
 "keys": [{"time": 0, "value": 1, "interp": "Cubic", "tangentMode": "Auto", "tangentWeight": "None",
           "arriveTangent": 0, "arriveTangentWeight": 0, "leaveTangent": 0, "leaveTangentWeight": 0}]}
```

- 같음(대입 묶음의 무동작 판정·차이 저장)은 리플렉션 값 기준이다. UE `FRichCurveKey::operator==`는 가중치를 비교하지 않고 접선은 Cubic 키에서만 비교하며(RC 222–229), `FRichCurve::operator==`는 기본값을 비교하지 않고(RC 1256–1277), `FStringCurveKey`는 `FString ==`라 대소문자를 무시한다(`StringCurve.cpp` 11–14). Rocks는 모든 칸을 비교한다. 실수는 비트로, String은 byte로 비교한다. Name은 Rocks Name의 같음 규칙(ASCII 대소문자 무시 — `Name.h` 89행)을 따르므로 `NameStepKey`는 대소문자만 다른 값을 같다고 본다(UE `FName`도 같다). 표시 철자는 wire에 보존된다(15절 10).

### 9.1 struct 검증 hook — D7

정렬 불변식과 유한 키 시간, `PackedTangentCurve`의 칸 사이 정합(11.4절)은 리플렉션 경로(decode, JSON·명령행 import, 대입 묶음, 경로 쓰기 `keys[3].time=…`)가 struct의 의미를 모른 채 바꿀 수 있다. 그래서 리플렉션 기반에 struct 검증 hook을 더한다([리플렉션 보강](../ReflectionExtensions.ko.md)에 넣을 변경 — CV3).

- **선언**: `ROCKS_STRUCT`가 `static Result<void> Validate(const S&) noexcept`를 가지면 MetaCompiler가 `Migrate`처럼 감지해 `RuntimeStructOps`의 새 칸 `validate`에 잇는다. 시그니처가 다르면 RMC 진단이다(번호는 CV3에서 15절 진단 표에 더한다). Binds 미러는 미러 struct에 `static Result<void> Validate(const 실제타입&) noexcept`를 둔다(12절).
- **계약**: noexcept, 할당하지 않는다(Allocator를 받지 않는다), 전역 상태·Registry를 읽지 않는 순수 함수, 아무 스레드에서나 불릴 수 있다(decode가 worker에서 돌 수 있다 — AssetSystem 341행). 결과는 Success 또는 InvalidArgument다. 다른 오류 코드를 내면 InvalidArgument로 다룬다. 값을 고치지 않는다(정렬해 주지 않는다 — G01).
- **부르는 때 — 게시 전 1회**: 바뀐 값이 객체에 게시되기 전, 그 값 안의 hook 있는 struct 인스턴스마다 한 번 부른다. 실패하면 게시하지 않고 값은 그대로다(진단 경로는 그 struct의 위치).
  - graph decode: 2단계에서 struct 값을 다 채운 뒤 게시 전. 마이그레이션(`Migrate`)이 먼저다.
  - JSON·명령행 import와 대입 묶음: 준비한 사본(상자)에 모든 대입을 적용한 뒤 Publish 전.
  - 경로 쓰기: 쓰기 대상을 담은 hook 있는 struct를 사본에 적용해 검증한 뒤 게시한다. 제자리 쓰기 경로는 이 단계에서 사본 경로로 바꾼다(hook 없는 struct는 지금 경로 그대로).
- **중첩 위치**: 바뀐 위치를 감싸는 hook 있는 struct 전부를 안쪽부터 부른다(경로 쓰기는 경로 위의 조상 struct들, decode·import는 값 트리 안의 모든 hook 있는 인스턴스). 위치는 struct 필드·struct 멤버·Array·StaticArray 원소·Optional 내부·Map 값·InstancedStruct 내용 전부다. InstancedStruct 내용은 담긴 실제 타입의 hook을 부른다.
- **부르지 않는 때**: C++가 직접 만든 값(API가 불변식을 지킨다), encode(게시된 값은 이미 검증되었다), 같은 값의 무동작 대입.
- **비용**: hook은 값 크기에 선형이다. 큰 커브의 키 하나를 경로로 쓰면 그 커브 전체를 다시 본다(편집기 쓰기 빈도에서 문제 없다).
- PlainCurve의 Cubic 금지는 hook이 아니라 타입으로 막는다(`PlainCurveInterp`에 Cubic이 없다 — 미선언 enum 값은 이미 decode가 거부한다).

### 9.2 wire 크기 — D8

키 배열은 `Array<Key>`(RecordList wire)다. 키 하나는 RecordList 원소 길이(u64) 8 byte + Record 머리말 44 byte + 필드 TLV 머리 필드마다 28 byte(Guid 16 + wire 4 + 길이 8 — `Record.h` 68–75) + 페이로드(float 6개 24 byte, enum 3개 I64 24 byte — `FieldValueWire.h` 29–46)로 약 350 byte다. UE의 수동 이진 키 직렬화(키당 27 byte — RC 163–181)의 약 13배다(개정 1의 "2–3배"는 틀렸다). 기본값과 같은 필드를 생략하는 인코딩이 있으면 줄지만 머리말만으로 52 byte다.

- 저작 데이터는 편집·텍스트·차이 저장의 편의를 위해 리플렉션 wire를 쓰고, 런타임에 많이 읽는 데이터는 압축 형태(11절)를 쓴다는 방향은 유지한다.
- **CV5 실측(17.4절 C14)**: 키당 `TangentCurveKey` 352 byte(원소 길이 8 + Record 머리말 44 + 필드 9 × 28 + 페이로드 48 — 위 추정과 같다), `PlainCurveKey` 116 byte(8 + 44 + 2 × 28 + 8)다. UE 수동 이진 키(27·8 byte)의 약 13·15배이고 1,000키 `TangentCurve`는 약 344 KiB다.
- **판단(권장안으로 확정)**: 저작 wire는 지금의 RecordList를 유지한다. 편집·텍스트·차이 저장·검증 hook이 키 단위 리플렉션에 기대고, 런타임에 많이 읽는 데이터는 `PackedTangentCurve`(11절, CV8)가 맡기 때문이다. 키마다 같은 머리말·필드 ID가 반복되므로 에셋 압축이 잘 들을 것으로 보지만 실측하지 않았다. 에셋 층(A 단계)에서 실제 커브 에셋의 크기가 문제로 측정되면 열 단위 배열(SoA)이나 리플렉션 일반의 기본값 필드 생략 인코딩을 다시 본다. 그때까지 별도 수동 codec을 두지 않는다.

## 10. 직렬화와 버전

- 각 struct는 리플렉션 버전 1에서 시작한다. 하위호환이 없는 초기 단계라 필드 의미를 바꾸면 버전을 올리고 이전 데이터는 버린다(마이그레이션을 쓰지 않는다).
- UE 에셋 파일의 직접 가져오기는 범위 밖이다(Rocks 에셋은 Rocks 형식으로 저작한다). UE 값과의 대조는 시험의 참조 구현으로 한다(16절).
- 키 핸들 표는 저장하지 않는다(8절).

## 11. 압축 커브 `PackedTangentCurve`

UE `FCompressedRichCurve`(RichCurve.h 362–463, RC 1279–2530)의 형식 결정·키 시간 양자화·키 데이터 배치를 따른다. 쿡된 애니메이션 커브처럼 많이 읽는 런타임 데이터용이고 저작 데이터는 `TangentCurve`다. 만드는 길은 `TryPack(const TangentCurve&, errorThreshold = 1e-4f, sampleRateHz = 120)`과 decode뿐이다(칸은 private PROPERTY).

### 11.1 형식 결정 — UE `FindRichCurveCompressionFormat`(RC 1279–1330)을 고친 것(D14)

- 키 없음 → Empty. 기본값을 `Optional<float>`로 옮긴다(UE는 `ConstantValue`에 `MAX_flt` sentinel — RC 1490·2144).
- **Constant**: 키가 1개이거나, Cubic 키가 없고 모든 키 값이 첫 키와 **비트까지 같은 유한 값**이며 앞·뒤 외삽이 모두 Linear가 아니면 Constant다(외삽·유한 조건은 17.7절 P2). UE는 `IsConstant(1e-8)` 허용오차로 판정하고 `keys[0].value` 하나만 남긴다(RC 1286·1495). 그러면 1e-8 안의 값 차이가 사라지고, Linear 외삽의 기울기와 CycleWithOffset의 누적 오프셋은 시간·주기에 비례해 커지는 차이가 된다. Rocks는 비트 같음으로 판정해 원본과 결과가 같게 한다(15절 20).
- **Weighted**: 가중 구간이 하나라도 있으면 Weighted다. 가중 구간은 `keys[i].interp == Cubic`이고 `IsItNotWeighted(keys[i], keys[i+1])`가 거짓인 (i, i+1)이다(5.3절). UE는 "Cubic 키의 가중 방식이 None이 아님"만 보아(RC 1296–1302) Linear·Constant 키의 도착 가중이 앞 Cubic 구간을 가중으로 만드는 경우를 놓친다 — 원본은 가중 경로(CE 223)로 평가하는데 압축은 Mixed·Cubic 형식이 되어 가중을 버린다(RC 1849–1852). Rocks는 구간 단위로 판정한다(15절 21). 마지막 키의 가중 방식처럼 어느 구간에도 쓰이지 않는 칸은 판정에 쓰지 않는다.
- 그 밖: 보간이 섞였으면 Mixed, 아니면 첫 키 보간에 따라 Linear·Cubic. 모든 키가 Constant여도 Mixed다(UE와 같다, RC 1320–1321). UE `RCIM_None` 갈래는 없다(D4).

### 11.2 키 시간 형식 — RC 1332–1475

기본 오차 1e-4·표본 120 Hz(`RichCurve.h` 334)로 16비트 양자화가 오차 안이고 크기가 작으면(`SizeUInt16 < SizeFloat32`, RC 1464–1467) Quantized16, 아니면 Float32. 양자화 식 `q = RoundHalfFromZero(Clamp((t − min) × (1/Δ), 0, 1) × 65535)`(RC 1634–1635), 복원 `(q × (1/65535)) × Δ + min`(RC 1720–1721). 표본 검사에서 UE는 표본마다 키 index를 한 칸만 전진해 표본 간격보다 촘촘한 키에서 판정이 틀린다(RC 1399–1426) — Rocks는 원본 쪽과 손실 쪽을 모두 `Evaluate`와 같은 평가(같은 구간 탐색·외삽 틀)로 비교한다(15절 11, 17.7절 P4). 시간이 모두 같은 키 2개 이상(Δ = 0)은 Float32로 둔다(UE는 `1/0`과 NaN Clamp로 판정한다).

### 11.3 바이트 배치

보간 배열 → 키 시간 → 키 데이터 순서다. 정렬 틈은 0이다. **모든 수는 리틀 엔디언으로 명시해 쓴다**(UE는 원시 메모리 그대로 — 15절 12).

- 보간 배열: Mixed는 키당 1 byte, Weighted는 키당 2 byte(보간·가중 방식). 보간 byte는 `CurveInterp` 값이 아니라 **형식 값**이다: Linear → 2, Cubic → 3, Constant → 1(UE `RCCF_*` — RC 1577–1588·1598–1611). 가중 byte는 `CurveTangentWeight` 값(0–3)이다.
- 키 시간: Quantized16은 2 byte 정렬 뒤 uint16 × N, 다시 4 byte 정렬 뒤 min·Δ float 두 개. Float32는 4 byte 정렬 뒤 float × N.
- 키 데이터: 키별 교차 배치 float. 키당 Linear 1(값)·Cubic 3(값·도착·출발)·Mixed 3(모든 키에 접선 — UE `MIXEDKEY_STRIPS_TANGENTS` 0)·Weighted 5(값·도착·출발·도착 가중치·출발 가중치).

### 11.4 리플렉션 표현과 정합 — D18, D21

- `ROCKS_STRUCT` 하나에 private PROPERTY `format`(`PackedCurveFormat`)·`keyTimeFormat`·앞·뒤 외삽(enum), `keyCount`(uint32), `constantValue`(float — Constant 형식), `emptyDefault`(`Optional<float>` — Empty 형식), `packed`(`Array<uint8>`)를 둔다. UE의 union 대신 칸을 나눈다(D13). 기본 외삽은 Constant다(UE `FCompressedRichCurve`만 `RCCE_None`인 것과 맞추지 않는다 — Rocks에는 None이 없다).
- `packed`는 D18에 따라 wire Bytes 블록 하나(u64 길이 + byte)로 저장된다(CV4 — 17.3절). 길이 한도는 `MaxElements = 1048576`(MaxArrayElements)이다.
- **정합 규칙**(`ExpectedPackedSize(format, keyTimeFormat, keyCount)`가 11.3절 배치로 기대 크기를 계산한다):
  - Empty: keyCount 0, packed 비어 있음. Constant: keyCount 0, packed 비어 있음, constantValue 사용.
  - Linear·Cubic·Mixed·Weighted: keyCount ≥ 2, `packed.Size() == ExpectedPackedSize(…)`(크기 계산의 넘침은 불일치로 본다).
  - enum 칸은 선언 값만(decode가 이미 거부한다).
- **검증 hook(Validate)**: 위 정합에 더해 보간 byte ∈ {1, 2, 3}, 가중 byte ∈ {0..3}, 키 시간 비내림차순·유한, Quantized16의 min·Δ 유한·Δ ≥ 0을 본다. 위반은 InvalidArgument다.
- **평가 진입의 O(1) 대조**: `Evaluate`는 매번 enum 범위와 `packed.Size() == ExpectedPackedSize(…)`(Linear 이상은 keyCount ≥ 2 포함)를 확인한다. 불일치면 **quiet NaN**을 낸다(조용한 대체값을 내지 않는다 — G01). `TryEvaluate`는 같은 경우 InvalidArgument를 낸다. hook·private 칸 때문에 정상 경로에서는 일어나지 않고, 메모리 밖 읽기를 막는 방어선이다. 정렬되지 않은 키 시간은 O(1)로 볼 수 없지만 이진 탐색이 범위 안 index만 쓰므로 메모리 안전하다(hook이 decode에서 거부한다).

### 11.5 평가와 풀기

- 평가: 압축 형태를 풀어 키 쌍을 만든 뒤 **5절과 같은 `math::EvalCurveSegment`를 부른다**. 11.1절의 형식 결정 수정과 함께 Float32 시간 형식의 압축 커브는 원본 `TangentCurve`와 비트가 같다(D14). UE 압축 평가는 제어점 곱셈 순서 `tangent × (diff × 1/3)`(RC 2108–2112)와 가중 경로의 순환 오프셋 누락(RC 2118 — 바로 반환) 때문에 원본과 다를 수 있다. Quantized16은 복원된 시간으로 평가하므로 그 시간 오차만큼 다르다.
- UE 결함 수정: Weighted 형식의 가중 판정에 키 index 대신 데이터 위치(index × 5)를 넘겨 엉뚱한 byte를 읽는 것(RC 2077–2105), 가중 경로가 순환 오프셋을 더하지 않는 것, 풀기(`PopulateCurve`)가 Cubic·Mixed 키를 Auto 접선으로 만들고 `SetKeys`의 `AutoSetTangents`로 접선을 다시 계산해 버리는 것, Empty 형식에서 union을 키 수로 읽어 기본값이 `MAX_flt`(비트 0x7F7FFFFF)면 약 21억 번 도는 것(RC 2441–2452), Constant 형식을 풀지 않는 것(RC 2443) — 15절 13.
- 풀기 `TryUnpack(TangentCurve&)`: 접선 방식은 User(Weighted는 가중 포함)로 복원하고 외삽·기본값도 옮긴다(UE는 옮기지 않는다). Constant 형식은 그 값의 키 하나(시간 0, Linear)로, Empty는 키 없음 + 기본값으로 푼다. 원래 접선 방식(Auto·Break)은 압축에서 사라진다.
