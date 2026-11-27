# 커브 에셋 — 5. 임포트·쿠킹·도구

[목차](README.ko.md)

## 5. 임포트·쿠킹·도구

### 5.1 임포터(A2f — AssetSystem 7.1절 18)

Developer `CurveAssetTools`가 다섯 임포터를 등록한다. UE `UCSVImportFactory`는 한 팩터리가 대화상자로 종류(DataTable·CurveTable·CurveFloat·CurveVector·CurveLinearColor)를 고른다(`UEd/Private/Factories/CSVImportFactory.cpp:89-92`, `209-250`). Rocks 임포터는 주 출력 타입이 고정이므로 종류마다 하나다. 기본 확장자는 두지 않고 `--importer=`로 고른다(CAD6). `csv`·`json`은 데이터 표 같은 다른 종류도 쓰는 형식이라서다. UE가 대화상자로 묻는 것과 같은 효과다.

| 이름 | 출력 | 원본 | 옵션 | UE |
|---|---|---|---|---|
| `curve_table_csv` | `CurveTableAsset` | CSV | `interp: CurveInterp = Linear` | `CreateTableFromCSVString`(`CurveTable.cpp:656-760`) |
| `curve_table_json` | `CurveTableAsset` | JSON | `interp: CurveInterp = Linear` | `CreateTableFromJSONString`(`:847-1003`) |
| `float_curve_csv` | `FloatCurveAsset` | CSV | 없음 | `UCurveBase::CreateCurveFromCSVString`(`CurveBase.cpp:131-191`) |
| `vector_curve_csv` | `VectorCurveAsset` | CSV | 없음 | 〃 |
| `color_curve_csv` | `ColorCurveAsset` | CSV | 없음 | 〃 |

#### 5.1.1 공통 규칙

- **재임포트**(UE `UReimportCurveTableFactory`·`UReimportCurveFactory`)는 A2f 공통 경로를 탄다.
  - 옵션 `interp`는 ImportRecord가 보존한다. UE의 편집기 전용 필드 `ImportCurveInterpMode`가 하던 역할이고, 에셋에는 두지 않는다.
- **임포트가 쓰는 필드**: A2f 규칙대로 임포트가 정하는 필드는 매번 모두 쓴다.
  - 표: 모드와 두 맵을 다시 만든다.
  - 단일 커브: 모든 채널의 키를 비우고 다시 넣는다(UE `ResetCurve`).
  - 임포트가 정하지 않는 필드는 그대로 둔다(기본값·외삽·색 조정·`eventCurve`).
  - UE 팩터리는 단일 커브를 기존 객체 자리에 `NewObject`로 다시 만든다(`CSVImportFactory.cpp:383`). 그래서 임포트하지 않은 필드도 기본값으로 돌아갈 수 있다(확실치 않음 — 7.5절). Rocks는 제자리 갱신이라 그 필드를 지킨다(◇ 7.2절 13).
- **텍스트 디코딩**: UE `LoadFileToString`처럼 BOM을 본다. UTF-16 변환은 Core `TryAppendUtf16AsUtf8`(`Core/Text/Utf16.h`)이다.
  - UTF-8 BOM이 있으면 뗀다. Excel "CSV UTF-8"이 BOM을 쓴다.
  - UTF-16 LE·BE BOM이 있으면 UTF-8로 바꾼다(Core 문자 변환).
  - BOM이 없으면 UTF-8로 읽는다.
  - 올바르지 않은 UTF-8·UTF-16 byte는 `CorruptData`다.
- **오류 등급**(에셋 세션 규약 — AssetSystem 19장):
  - 사람이 쓴 텍스트의 형식 오류는 `InvalidArgument`다. 진단은 `InvalidSource`, detail은 아래 위치다.
  - 행 단위 문제는 UE처럼 그 행(JSON은 그 항목 — 5.1.3절)만 건너뛰고 경고를 남긴다. 경고 진단은 새로 더할 `CurveImportRowSkipped`다(AssetDiagnosticCode — CA3에서 에셋 세션과 합의).
  - UE가 조용히 0으로 읽는 숫자 아닌 칸은 실패다(★ 7.2절 5).
  - UE가 빈 표로 끝내는 경우도 실패이고, 기존 에셋을 지킨다(★ 7.2절 6). 시간 중복이나 행 부족이 그 예다. UE 팩터리는 임포트 전에 기존 표를 `EmptyTable()`한다(`CSVImportFactory.cpp:354-360`).
- **위치(detail)**:
  - CSV는 그 행이 시작하는 원본의 물리 줄 번호(1부터)다. 빈 줄 건너뛰기나 따옴표 안 줄바꿈이 있어도 사람이 편집기에서 찾는 줄이다.
  - JSON은 최상위 배열 원소 번호(1부터)다. 문서 오류(파싱 실패·배열 아님·빈 배열)는 0이다.
  - 할당·용량 실패(이름 intern 포함)는 원본 탓이 아니라 진단 없이 그 오류를 돌려준다.
- **숫자 칸**:
  - 앞뒤 ASCII 공백·탭을 떼고 Core 엄격 파싱(`TryParseDouble` — 칸 전체 소비)으로 double을 읽어 float로 바꾼다(UE Windows `Atof` = `(float)_tstof` — 같은 이중 반올림, 7.2절 20). UE 파서는 칸을 trim하지 않지만 `Atof`가 앞 공백을 건너뛴다. 그래서 `1, 2`가 읽혀야 한다.
  - 비유한 값(`inf`·`nan`)은 시간·값 모두 실패다(★ 7.2절 5).
- **표 모드**:
  - `interp`가 Cubic이면 Tangent 모드다. 행 커브의 키는 모두 보간 Cubic이다.
  - Linear·Constant면 Plain 모드다. 커브 하나에 보간 하나(`PlainCurveInterp`)다.
  - UE `:676`과 같다. Rich는 키마다 `SetKeyInterpMode`를 부르고, 이 함수가 자동 접선까지 계산한다(`RichCurve.cpp:520-531`).
  - Rocks는 키를 넣을 때마다 보간을 정하고, 행의 키를 다 넣은 뒤 `AutoSetTangents`를 한 번 부른다. UE `AutoSetTangents`(`RichCurve.cpp:680-755`)는 첫 키의 도착·끝 키의 출발·Linear 안쪽 키의 접선을 옛 값으로 두지만, 임포트는 키를 더하기만 하고(시간 중복은 미리 거른다) 안쪽 키가 끝 키로 바뀌지 않으므로 키마다 부른 UE의 최종 접선과 같다.

#### 5.1.2 CSV 표(`curve_table_csv`)

- **첫 행은 시간 축이다.** 0열(UE 예 `---`)은 무시하고 1열부터 시간이다.
  - 시간이 숫자가 아니거나 비유한이면 실패한다.
  - 같은 시간이 둘이면 실패한다(UE `:686-689`).
  - 시간 열이 0개(머리 행이 0열뿐)면 UE처럼 받는다. 이후 행은 키 없는 커브가 된다(`:623-635`).
  - 행이 1개 이하이면 실패한다(UE "Too few rows.").
- **다음 행부터 0열은 행 이름이고, 1열부터 시간 순서대로 값이다.**
  - 이름에서 Name 금지 문자를 뺀다. 상수 `CurveTableInvalidNameCharacters`는 UE `INVALID_NAME_CHARACTERS` = `"' ,\n\r\t`이다(UE `MakeValidName` — `CurveTable.cpp:79-100`). 뺀 뒤 이름이 비면 그 행을 건너뛴다.
  - 이름이 이미 있으면 건너뛴다.
  - 열 수가 시간 열 수 + 1과 다르면 건너뛴다.
  - 값 칸이 숫자가 아니면 실패한다.
- 키를 넣는 순서와 보간 설정은 UE와 같다(`:729-752`).

#### 5.1.3 JSON 표(`curve_table_json`)

- 최상위는 배열이고, 원소는 `{"Name": "행", "<시간>": 값, …}`이다(UE `:847-1003`).
- 읽기는 Core JSON 읽기(`Core/Text/Json.h`)다. 배열과 객체 멤버를 원문 순서로 준다.
- **문서 오류는 실패한다.**
  - 파싱 실패
  - 최상위가 배열이 아님
  - 빈 배열
  - 한 객체 안 중복 키: Core JSON이 문서 오류로 낸다(`Json.h:22, 175`). UE `FJsonObject`는 뒤의 값이 이긴다(◇ 7.2절 14).
- **행 이름**: 멤버 키 `Name`을 대소문자 무시로 찾고(UE `FJsonObject` 키 비교), 여럿이면 뒤의 것이다(UE `TMap` 덮어쓰기). 값은 UE `GetStringField`(`AsString`)처럼 문자열 그대로, bool은 `true`·`false`, 숫자는 원문 토큰이다(UE `SanitizeFloat` 서식과 다를 수 있다 — 7.2절 18).
- **원소 오류는 그 원소를 건너뛰고 경고를 남긴다.**
  - 원소가 객체가 아님
  - `Name`이 없음(이름이 비거나 None)
  - 이름 중복(대소문자 무시)
- **항목 오류는 그 시간 항목만 건너뛰고 경고를 남긴다.** 행은 남는다(UE `:931-944` 안쪽 `continue`).
  - 시간 키가 float로 읽히지 않음
  - 값이 숫자로 읽히지 않음
- **한 행 안에 같은 시간이 둘이면 그 행 전체를 건너뛰고 경고를 남긴다**(UE `FindDuplicateXValues` → 행 `continue` — `:962-985`).
- **값으로 받는 것**은 UE `TryGetNumber`와 같다(`JsonValue.h:451-462`, `JsonValue.cpp:476-480`).
  - 숫자(double을 거쳐 float)
  - UE `FString::IsNumeric` 규칙의 문자열(`[+|-]` 뒤 숫자와 `.` 하나 — 지수·공백 없음, 숫자가 없는 `.`·`-`는 0)
  - bool(참 1, 거짓 0)
  - 비유한 값은 실패한다.
- 시간 키는 숫자 칸 규칙(앞뒤 공백 trim·칸 전체)이다. UE `LexTryParseString`은 `Atof` 결과가 0이 아니면 뒤 쓰레기(`2abc`)도 받는다(★ 7.2절 17).
- 키 순서는 객체 멤버 순서다.
- UE는 시간 중복 행에서 커브를 지우지 않는 누수가 있다(`:982-985`). Rocks에는 해당하지 않는다.

#### 5.1.4 CSV 단일 커브(`*_curve_csv`)

- 머리 행이 없다. 행마다 `시간, 채널0, 채널1, …`이다. 채널 수는 스칼라 1, 벡터 3, 색 4이고 순서는 2장의 채널 순서다.
- 행 처리는 UE와 같다(`CurveBase.cpp:150-185`). 해당 행마다 경고를 남긴다.
  - 칸이 2개 미만이면 그 행을 건너뛴다.
  - 칸이 채널 수 + 1보다 많으면 남는 칸을 버린다.
  - 칸이 적으면 있는 채널에만 키를 넣는다.
- 키는 늘 Linear이고, 자동 접선을 계산한다(UE `SetKeyInterpMode(Linear)` — `:165-170`).
- 행이 하나도 없으면 실패한다(UE "No data.").

### 5.2 Core `CsvReader`(UE `FCsvParser`와 같은 규칙)

- 위치는 `Core/Text/Csv.h`다.
  - 서명: `Result<void> TryParseCsv(StringView text, Array<CsvRow>& out)`(out의 allocator로 만든다).
  - `CsvRow`는 칸의 사본(`Array<String>` — 따옴표 해제 결과)과 그 행이 시작하는 물리 줄 번호를 담는다.
- 파일을 UE와 같은 칸으로 나누려고 규칙을 UE `FCsvParser`(`Runtime/Core/Private/Serialization/Csv/CsvParser.cpp`)와 같게 둔다. RFC 4180보다 관대하다.
  - 빈 줄은 건너뛴다(`:40-45`). 공백만 있는 줄은 건너뛰지 않는다.
  - 따옴표는 칸 맨 앞에서만 인식한다(`:64`). `""`는 따옴표 하나로 해제한다(`:63-100`).
  - 닫는 따옴표 뒤 글자는 칸에 이어 붙인다. 예: `"ab"cd` → `abcd`(`:95-99, 125-131`).
  - 닫히지 않은 따옴표는 문자열 끝까지, 줄바꿈·쉼표를 포함해 한 칸으로 삼킨다. 오류로 보지 않는다(`:74-101, 135`).
  - NUL byte는 문자열 끝이다. 비지 않은 입력은 첫 byte가 NUL이어도 빈 칸 하나짜리 행이 생긴다(UE와 같다).
  - 줄 끝은 `\r\n`·`\r`·`\n`이다(`:138-152`).
  - 끝 쉼표는 빈 칸 하나를 더한다(`:114-122`).
  - 칸을 trim하지 않는다. 숫자 칸 trim은 임포터가 한다(5.1.1절).
- 오류는 할당 실패(OutOfMemory)뿐이다.

### 5.3 쿠킹

- `FloatCurveAsset`·`VectorCurveAsset`·`ColorCurveAsset`은 기본 처리기다(A4c2 — 필드 그대로). UE도 커브 에셋에 쿠킹 변형이 없다(tagged property 그대로).
- `CurveTableAsset`은 처리기 `CurveTableCookProcessor`(Developer `CurveAssetTools`)를 쓴다. 에디터 객체가 아니라 쓰기용 사본에서 다음을 한다(★ 7.2절 7).
  1. 모드가 쓰지 않는 맵을 비운다. 비어 있지 않았으면 경고 `CurveTableUnusedRows`를 남긴다(3.2.1절).
  2. 행마다 중복 키를 제거한다(UE `CurveTable.cpp:215-232` — `CurveTable.RemoveRedundantKeys` CVar 기본 1).
     - Plain은 `TryRemoveRedundantKeys(0)`다(UE `FSimpleCurve::RemoveRedundantKeys(0.f)`).
     - Tangent는 표본 속도 없이 `TryRemoveRedundantKeys(0)`를 부르고, 키가 3개 이상이었으면 이어서 `AutoSetTangents()`를 부른다.
       - UE `RemoveRedundantAutoTangentKeys(0.f)` = `RemoveRedundantKeys(0, FFrameRate(0, 0))`다(`RichCurve.cpp:916-929`).
       - UE는 키가 3개 미만이면 바로 반환한다. 그렇지 않으면 `RemoveRedundantKeysInternal` 끝에서 `AutoSetTangents`를 부른다(`:1094`).
       - Rocks `TryRemoveRedundantKeys`는 접선을 다시 계산하지 않는다(CurveData D10). 그래서 처리기가 그 차이를 메운다.
- 끄는 설정은 두지 않는다. UE 기본은 켬이고, 끄는 요구가 생기면 쿠킹 설정 키를 더한다(◇ 7.2절 10).
  - 처리기 버전 Guid가 이 동작의 정체성이다. 동작을 바꾸면 버전을 올려 너겟 키를 바꾼다(AssetSystem 10.2절).
- `ColorCurveAtlas`는 4.3절 처리기다(`Texture2D` 처리기를 감싼다).

### 5.4 새로 만들기·명령행

- `asset-new FloatCurveAsset <path>` 등은 빈 커브를 만든다(2.4.4절 기본 키 — CAD4).
- 키 편집은 지금 `asset-set` 경로 텍스트(리플렉션 텍스트 — 키 배열)로 한다.
  - 전용 키 편집 명령은 두지 않는다.
  - 커브 편집기는 Studio다.
