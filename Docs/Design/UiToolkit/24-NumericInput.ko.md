## 24. 숫자 입력(UW4n — 9.1절)

9.1절 UW4n(사용자 요청 2026-11-26 — "숫자로도 입력되고 컨트롤 안에서 마우스 드래그로 min/max 범위 내에서 조절되는 것")의 세부 설계다. UE `SSpinBox`·`SNumericEntryBox`·`SNumericVectorInputBox`(`SVectorInputBox`)·`INumericTypeInterface`·`FBasicMathExpressionEvaluator`·`FUnitConversion` 수준으로 만든다. 근거는 UE 5.8 로컬 클론이다(Slate/Public/Widgets/Input/SSpinBox.h·Private/.../SSpinBox.cpp·SNumericEntryBox.h·SVectorInputBox.h·NumericTypeInterface.h, Core/Private/Math/BasicMathExpressionEvaluator.cpp·UnitConversion.cpp, SlateCore/Private/Styling/StarshipCoreStyle.cpp 611–645행). 순서는 24.9절이다.

### 24.1 지금 상태

- `SpinBox`(Frame 파생, float)는 끌기·위아래 키·글자 입력(버퍼 직접 편집)이 있다. 모양은 어두운 단색 바탕이고 고정 크기 72×24다. 스타일·채움 막대·슬라이더 범위·지수·배수 키·휠·커서 숨김이 없다.
- 표시 글자는 호출자 caption delegate가 shaping한다(편집기가 아니다). 수식·단위·여러 값 미정 표시가 없다.
- 커서 잠금 계약(23.5절 — `PointerReply::lockCursor`·`PointerEvent::delta`·`releaseCursorAt`)과 편집 텍스트(22장)가 이제 있다. 이 장은 그 둘 위에 선다.

### 24.2 숫자 형식·해석(`NumericFormat` — UE `TDefaultNumericTypeInterface`)

- 위치: `UiFramework/Numeric/`(UI 밖에서 쓸 일이 생기면 그때 옮긴다 — Core에는 수학·컨테이너만, 4장).
- **값 표현**: 값은 `double`이고, 형식 `NumericKind`(`Float`·`Double`·`Int32`·`Int64`·`UInt32`·`UInt64` 등 UE 인스턴스 목록과 같다)가 반올림·범위를 정한다. 정수 형식은 `RoundIfIntegerValue`처럼 `Floor(v + 0.5)` 뒤 형식 범위로 자른다. 64비트 정수는 2^53을 넘으면 double 정밀도를 넘는다 — UE도 같은 double 경로라 같은 한계다(`SSpinBox::InternalValue`가 double).
- **문자열로**(`ToString`): 묶음 기호 없음, 소수 자리 최소·최대(`MinFractionalDigits` 기본 1·`MaxFractionalDigits` 기본 6 — UE `DefaultMinFractionalDigits`·`DefaultMaxFractionalDigits`), 정수 형식은 소수 없음, 거의 정수 표시(`IndicateNearlyInteger`, 기본 끔 — 자른 값이 정수와 거의 같지만 같지 않으면 소수 뒤에 "..."를 붙인다, UE FastDecimalFormat 517–597행: 1e-16이 "0.0"으로만 보여 0으로 오해하지 않게)를 따른다. 지역화 숫자 규칙은 지금 고정 "." 소수점이다(UE는 `ShouldUseLocalizedNumericInput` 설정 — 지역화 단계에서 더한다).
- **문자열에서**(`FromString(text, existing)`): (1) 전체가 숫자면 그 값, (2) 아니면 수식으로 계산(24.3절), (3) 실패면 없음(값을 바꾸지 않는다). 결과는 형식 범위로 자른다.
- **입력 가능 글자**(`IsCharacterValid`): `0-9 ( ) - + = \ / . , * ^ %`와 공백, 단위 입력이면 단위 글자(24.4절). 편집 중 잘못된 글자가 들어오면 그 앞까지만 남긴다(UE `TextField_OnTextChanged`).

### 24.3 수식(`NumericExpression` — UE `FBasicMathExpressionEvaluator`)

- 문법은 UE 그대로다: 숫자, `( )`, 앞 단항 `+ - sqrt`, 이항 `^`(우선순위 3), `* / %`(4, 왼쪽 결합), `+ -`(5, 왼쪽 결합). 공백은 무시한다.
- **상대 입력**: 처음이 `+= -= *= /=`이면 지금 값을 앞에 넣는다("*=2" → 지금 값 × 2).
- 오류: 0으로 나눔·0으로 나머지·문법 오류는 실패(값 유지). UE는 `sqrt`·`^`·`%`를 float로 계산한다(`@TODO FLOATPRECISION`) — Rocks는 double로 계산한다(UE보다 정확 — 결과 차이는 float 반올림만큼).
- 재귀 깊이·토큰 수 상한(`MaxNumericExpressionTokens` 256)을 둔다 — 붙여 넣은 긴 글자가 스택을 넘지 않게.

### 24.4 단위(`NumericUnit` — UE `EUnit`·`FUnitConversion`·`TNumericUnitTypeInterface`)

- 단위 목록·순서는 UE `EUnit` 전체(길이·각·속도·각속도·가속도·온도·질량·밀도·힘·토크·충격량·위치 충격량·소리·주파수·데이터 크기·광속·광도·조도·휘도·노출·시간·픽셀 밀도·배수·응력)와 `Unspecified`이고, 종류는 `NumericUnitType`이다. 표시 글자(UE `DisplayStrings` — µm·°·cm/s²·°C·KiB 등, UTF-8)와 입력 별칭(UE `ParseCandidates` — 이름·기호·µ/² 대체 글자)은 UE 표 그대로다.
- **계수**(UE `*UnificationFactor`): 종류마다 기준 단위로 곱했다가 대상 기준의 역수를 곱한다. 온도는 켈빈을 거치는 오프셋 변환이다. 소리·충격량·광도·조도·휘도·픽셀 밀도·노출은 같은 종류 안에서도 바꾸지 않는다(UE와 같다). UE가 float 리터럴이나 근사로 둔 값은 정확한 값으로 둔다: 인치 0.0254 m(UE `1/39.3700787`), cm/s² 0.01(UE `0.01f`). cm↔m 계수는 Core `UnitConversion.h`의 `CentimetersPerMeter`를 쓴다(중복 금지).
- **가장 알맞은 단위**(UE `QuantizeUnitsToBestFit`·`GetQuantizationBounds`): 미터 길이·영국식 길이·미터 질량·영국식 질량·밀도·주파수·데이터 크기·시간(µs부터)·응력 묶음 안에서, 절댓값이 1보다 크면 위 단위로, 1 이하면 아래 단위로 옮긴다. UE는 rpm이 주파수 묶음 계산에서 잘못된 자리를 읽는다 — Rocks는 rpm을 옮기지 않는다.
- **단위 수식**(UE `FUnitExpressionParser` — 24.3절 수식과 다른 문법): 숫자(+ 공백 + 단위), `( )`, 단항 `+ -`, `* /`, `+ -`, 처음 `+= -= *= /=`(지금 값은 기본 단위 값). 단위 글자는 숫자 뒤에서 대소문자 무시로 가장 긴 별칭과 맞춘다("5min"은 분, "5m"은 미터).
  - 더하기·빼기: 한쪽만 단위가 없으면 그쪽을 기본 단위로 보고, 호환되면 왼쪽 단위로 바꿔 더한다. 호환되지 않으면 오류다.
  - 곱하기: 둘 다 단위면 오류, 결과는 단위 있는 쪽. 나누기: 오른쪽이 단위면 오류, 0이면 오류.
  - UE 단위 수식은 이항 연산이 오른쪽 결합이라("10 - 4 - 3" → 9) 결함이다 — Rocks는 왼쪽 결합이다. `^ % sqrt`는 단위 수식에 없다(UE와 같다).
  - 결과에 단위가 없으면 기본 단위다(UE `TryParseExpression`).
- **칸의 단위**(UE `TNumericUnitTypeInterface`): `NumericUnitOptions` — 값이 저장된 단위(`underlying`, 기본 `Unspecified`), 고정 표시 단위(`display`, Optional), 가장 알맞은 단위로 표시(`bestFit`).
  - 글자로: 단위가 `Unspecified`면 24.2절 그대로. 아니면 표시 단위(고정 → 가장 알맞은 → 저장 단위 순)로 바꿔 "값 + 공백 + 기호"다("100 cm"). 정수 형식이라도 다른 단위로 바꿔 보이면 소수를 남긴다(500 m를 km로 → "0.5 km". UE는 정수로 잘라 "0 km" — `ToString`이 형식 값으로 바꾼다).
  - 글자에서: 단위가 `Unspecified`면 24.2절. 아니면 단위 수식을 표시 단위(고정 또는 저장 단위)를 기본으로 계산하고, 결과를 저장 단위로 바꾼다. 호환되지 않으면 실패다. 정수 형식에도 double로 계산한 뒤 반올림한다("0.5km"를 m 정수 칸에 → 500).
  - 칠 수 있는 글자는 탭 말고 모두다(UE `IsCharacterValid` — 단위 글자).
- 전역 단위 설정(UE `FUnitSettings` — 표시 여부·종류별 표시 단위)은 에디터 설정과 함께 둔다(UW4h 이후). 지금은 칸마다 정한다.

### 24.5 스핀 상자(`SpinBox` 다시 만들기 — UE `SSpinBox`)

- **구조**: 바탕(스타일 brush)과 채움 막대를 직접 그리고, 자식은 `EditableText` 하나다(UE는 `STextBlock`과 `SEditableText`를 바꿔 보인다 — Rocks는 편집기 하나로 표시·편집을 함께 한다: 표시 모드는 읽기 전용·hit 투명·표시 글자, 글자 모드는 편집 가능·초점·전체 선택). 생성자는 편집 텍스트처럼 `TextShaper`와 `Platform`을 받는다. caption delegate는 없앤다(하위 호환 없음).
- **스타일 `SpinBoxStyle`**(UiCore, 핵심 키 "SpinBox"·"NumericEntrySpinBox"·"NumericEntrySpinBox_Dark" — StarshipCoreStyle 614–640행):
  - 바탕 RB(Input, 4, InputOutline, 1)·올림 RB(Input, 4, Hover, 1)·활성 RB(Input, 4, Primary, 1).
  - 채움: 활성 RB(Primary, 4, 투명, 1)·올림 RB(Hover, 4)·비활성 RB(Dropdown, 4).
  - 화살표 이미지 없음, 전경 ForegroundHover, 글자 여백 (7, 2.5, 7, 2.5), 안쪽 여백 1.
  - `_Dark`: 활성 채움 Hover·비활성 채움 Secondary·글자 여백 0, 활성 바탕 없음(올림 바탕을 쓴다).
  - 앱 스타일 "SpinBox"를 따르고 `SetSpinBoxStyle`이 명시하면 고정한다(23장 규칙).
- **인자 기본값**(UE `SSpinBox::FArguments`): 값 0, 값 범위 없음(형식 최소·최대), 슬라이더 범위 없음, Delta 0, Shift ×10·Ctrl ×0.1·Shift+Alt ×100·Ctrl+Alt ×0.01, 선형 감도 없음, 슬라이더 지수 1(중립값 없음), 휠 켬, 키마다 값 알림 끔, 내용 여백 (2, 1), 확정 때 초점 지우기 끔, 확정 때 전체 선택 켬, 최소 원하는 폭 0, 왼쪽 정렬, Esc 되돌림 켬.
- **그리기**(UE `OnPaint`): 활성(끌기 또는 글자 모드)·올림·보통 바탕. 슬라이더 범위가 둘 다 있으면 채움 막대: 값을 Delta에 맞춘 뒤 비율 `Fraction(값, 최소, 최대)`, 지수 ≠ 1이면 `1 - (1 - f)^지수`(중립값이 범위 안이면 양쪽 로그 곡선 — `SpinBoxComputeExponentSliderFraction`), 폭 × 비율에서 안쪽 여백을 뺀 크기, 글자 모드에서는 그리지 않는다.
- **끌기**(UE `OnMouseButtonDown·Move·Up`):
  - 왼쪽 누름: capture, 초점(Mouse), 끈 거리 0, 끌기 전 값 저장. 커서는 끌기가 시작될 때 잠근다(`lockCursor` — 23.5절, UE는 누름에서 `UseHighPrecisionMouseMovement`). 끌기 문턱(`PointerDragThreshold`)을 넘으면 끌기 시작(`OnBeginSliderMovement`).
  - 이동: 단계 = (범위 ≤ 10이면 0.1, 아니면 1) × 배수 키. 슬라이더 범위가 있으면 채움 비율에 `delta.x × 단계`를 더해(최소 폭 100) 값으로 되돌린다(지수 역함수 포함). 범위가 없으면 선형 감도·Delta가 있으면 `|dx| / 감도 × Delta^지수 × 단계`, 없으면 `|dx| / 폭 × max(|값|, 1)^지수 × 단계`. 슬라이더 범위·값 범위로 자른다. Alt는 동적 슬라이더 범위(UE `SupportDynamicSliderMaxValue`)를 늘린다.
  - 놓음: 끌었으면 Delta에 맞추고(배수 키를 누른 채면 맞추지 않음) 확정(`OnValueCommitted(Enter)`·`OnEndSliderMovement`). 커서는 누른 자리로 되돌린다(`releaseCursorAt` — UE `SetMousePos(CachedMousePosition)`). 끌지 않았으면 글자 모드로 들어가 편집기에 초점을 준다.
  - 끌기 중 Esc는 끌기 전 값으로 되돌리고 capture를 푼다(UE `OnKeyDown` Escape).
  - 커서 모양: 끌기 꺼짐 기본, 끄는 중 없음, 평소 좌우 크기 조절(UE `OnCursorQuery`).
  - 펜·터치는 같은 끌기다(커서 잠금 없이 위치 차이 — Rocks 보강, UE는 마우스만 이동량을 쓴다).
- **키**(UE `OnKeyDown`): 위·오른쪽 = +Delta(없으면 기본 단계 × 배수), 아래·왼쪽 = −, 확정(`ArrowKey` — 슬라이더 범위로 자르고 Delta에 맞춤) 뒤 표시 모드. Enter는 글자 모드. 초점이 Navigation·직접 지정으로 오면 글자 모드(UE `OnFocusReceived`).
- **휠**(UE `OnMouseWheel`): 켜져 있고 끄는 중이 아니며 글자 모드 초점일 때만. 단계는 `WheelStep` 또는 기본 단계(실수이고 범위 ≤ 10이면 0.1) × 배수.
- **글자 모드**: 편집기 글자는 값 문자열(표시 delegate와 따로 — UE `OnEditEditableText`). 잘못된 글자는 앞까지만. Enter 확정은 글자 모드를 유지하고(전체 선택), 초점 잃음 확정은 표시 모드로 돌아간다. 확정 값은 `FromString`(수식·단위)이고 값 범위로 자른다. Esc는 되돌림(편집 텍스트 22장).
- **확정 규칙**(UE `CommitValue`): Spin·ArrowKey는 슬라이더 범위로, 모두 값 범위로 자른다. Spin·ArrowKey(또는 `AlwaysUsesDeltaSnap`)는 Delta에 맞춘다. TypeIn·ArrowKey는 `OnValueCommitted`, 모두 `OnValueChanged`. 바인딩된 값이 끄는 사이 바깥에서 바뀌면 그 값을 따른다.
- **표시**: `OnGetDisplayValue`가 있으면 그 글자(단위 표시 포함), 없으면 `ToString`. 최소 원하는 폭(`MinDesiredWidth`)·정렬.
- **값 계약 이름**: `SpinCommitReason`은 `Enter`·`FocusLost`·`Spin`·`ArrowKey`·`TypeIn`(UE `ECommitMethod` + `ETextCommit`)으로 넓힌다. Widget 층 `WidgetSpinCommitReason` 미러도 같이 바꾼다.

### 24.6 숫자 입력 칸(`NumericEntryBox` — UE `SNumericEntryBox`)

- 값은 Optional이다. 없으면 미정 문자열(기본 "---", UE `DefaultUndeterminedString`)을 보이고, 글자를 넣으면 `OnUndeterminedValueCommitted`.
- `AllowSpin`(기본 끔): 켜면 안에 `SpinBox`(스타일 "NumericEntrySpinBox"), 끄면 편집 텍스트 상자(스타일 "NormalEditableTextBox").
- 레이블: 바깥(기본 — 칸 왼쪽 별도 칸, 여백 (3, 0)) / 안쪽(칸 위에 겹침 — 스핀이면 스핀 위). 레이블 만들기:
  - `BuildLabel(글자, 전경, 바탕)`: 꾸밈 brush 위 글자(여백 (1, 0, 6, 0), hit 투명). UE 꾸밈은 `Common/TextBoxLabelBorder` png — Rocks는 RB(바탕색, 4, 왼쪽 모서리만)로 그린다.
  - `BuildNarrowColorLabel(색)`: 2×16 막대(UE `NarrowDecorator` SVG — Rocks가 그린 SVG `CoreWidgets/NumericEntryBox/NarrowDecorator`), 여백 (2, 0, 0, 0).
- 토글(`DisplayToggle` — 체크 상자, 여백 (1, 0, 1, 0)): 끄면 칸을 비활성으로 보인다.
- 툴팁 형식(`ToolTipTextFormat` — "X: {0}")과 0에 아주 가까운 값 경고 툴팁(UE `NearZeroWarning`)을 따른다. 범위가 있으면 툴팁에 범위를 붙인다(Rocks 보강 — "0 – 100").
- 나머지 인자(배수·감도·Delta·자릿수·범위·슬라이더 범위·지수·휠·키마다 알림·최소 폭)는 스핀 상자로 넘긴다. 기본은 UE `SNumericEntryBox` 값이다(값 범위 형식 최소·최대, 슬라이더 범위 0–100, 휠 켬, 최소 폭 0).

### 24.7 벡터 입력(`VectorInputBox` — UE `SNumericVectorInputBox`)

- 성분 2–4개(X·Y·Z·W), 성분마다 `NumericEntryBox`(레이블 안쪽, 여백 3, 미정 "Multiple Values")를 가로로 나란히 둔다. 값은 Optional 벡터 또는 성분별 Optional이다(여러 개 고른 대상).
- `ColorAxisLabels`(기본 끔)면 성분마다 좁은 색 레이블이다. 축 색은 UE `AxisDisplayInfo::GetAxisColor` 기본값: X (0.594, 0.0197, 0), Y (0.1349, 0.3959, 0), Z (0.0251, 0.207, 0.85)(선형). W는 색 없음.
- 성분 이름(툴팁 "X: {0}"), 성분별 변경·확정·끌기 시작·끝 사건, 범위·슬라이더 범위 벡터, `SpinDelta`(기본 1)·선형 감도(기본 1), `AllowSpin`(기본 끔), 성분 순서(`Swizzle`), 토글.
- 비율 고정(UE `ConstrainVector`): 호출자가 성분 변경을 다른 성분에 퍼뜨리는 delegate를 준다.

### 24.8 갤러리와 Widget 층

- 갤러리 Widgets 탭 "Numeric Input" 묶음(UE 갤러리 Spin Box 행과 같은 자리): 정수, 실수(소수 2자리), 슬라이더 범위 채움(0–100), 지수 슬라이더, 단위(cm·°·%), 바깥 글자 레이블, 축 색 벡터(X·Y·Z), 여러 값 미정, 토글. 값은 옆 레이블에 보인다(16장 원칙). 이름 열은 Rocks 이름이다.
- Widget 층: `SpinBoxWidget`을 새 계약으로 바꾸고(모양은 "SpinBox" 스타일, 값·범위·슬라이더 범위·Delta·자릿수·배수·지수·휠·단위 속성), `NumericEntryBoxWidget`·`VectorInputBoxWidget`을 더한다(UMG `USpinBox`·에디터 `SNumericEntryBox` 대응). 새 리플렉션 필드는 MetaCompiler `ids add`로 등록한다.

### 24.9 시험과 단계

- 시험(규칙마다):
  - 형식·해석: 자릿수·정수 반올림·거의 정수, 숫자·수식(우선순위·결합·상대 입력·0 나눔·깊이 상한)·잘못된 글자.
  - 단위: 종류 안 변환(길이·각·온도 오프셋), 다른 종류 실패, 수식 안 단위, 가장 알맞은 단위, 표시 글자.
  - 스핀 상자: 스타일 키·값, 채움 비율(지수·중립), 끌기(문턱·단계·배수·범위 유무·선형 감도·Delta 맞춤·확정 사건·커서 잠금·되돌림 자리), Esc, 키, 휠(초점일 때만), 글자 모드(진입·확정·Esc·잘못된 글자·초점 잃음), 바깥 값 변경, 터치 끌기.
  - 입력 칸·벡터: 미정 표시·확정, 레이블 위치, 토글, 축 색, 성분 사건.
  - 갤러리 self-test: 끌기로 값이 바뀌고 글자 입력 "10+5*2"가 20이 된다.
- **UW4n1**: 숫자 형식·해석·수식(24.2–24.3절).
- **UW4n2**: 단위(24.4절).
- **UW4n3**: 스핀 상자 다시 만들기(24.5절) — 스타일, 편집기 자식, 끌기·키·휠·글자 모드, 커서 잠금.
- **UW4n4**: 숫자 입력 칸(24.6절).
- **UW4n5**: 벡터 입력(24.7절).
- **UW4n6**: 갤러리·Widget 층(24.8절).
- 리뷰어는 UW4b 2차 끝(UW4n6 뒤)에 한 번 둔다(9.1절).
