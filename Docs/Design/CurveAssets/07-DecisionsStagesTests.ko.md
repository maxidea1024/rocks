# 커브 에셋 — 7. 결정·UE와 다른 점·단계·시험

[목차](README.ko.md)

## 7. 결정·UE와 다른 점·단계·시험

### 7.1 결정

| ID | 항목 | 결정 | 근거·버린 대안 |
|---|---|---|---|
| CAD1 | 이름 | 에셋은 `FloatCurveAsset`·`VectorCurveAsset`·`ColorCurveAsset`·`CurveTableAsset`, 기반은 `CurveAsset`이다. 원천은 `*CurveSource`, 평탄 값은 `VectorCurveValue`·`ColorCurveValue`, 행 참조는 `CurveTableRowRef`, atlas는 `ColorCurveAtlas`다 | 값 커브(`TangentCurve`)·애니메이션 커브(UE `FVectorCurve` — 애니메이션 단계)와 헷갈리지 않게 에셋 이름에 `Asset`을 붙인다. atlas는 텍스처라 붙이지 않는다(`Texture2D`처럼) |
| CAD2 | 모듈 | Runtime `CurveAssets`(Curves·AssetRuntime)와 Runtime `CurveAtlas`(+Texture), Developer `CurveAssetTools`·`CurveAtlasTools`, Core `CsvReader` | 커브 소비자가 텍스처 모듈을 끌어오지 않는다. CSV 읽기는 범용이라 Core에 둔다 |
| CAD3 | 표 저장 | 모드 하나와 맵 둘(`OrderedMap<Name, PlainCurve>`·`OrderedMap<Name, TangentCurve>`)을 두고, 모드가 쓸 맵을 정한다. 쓰지 않는 맵은 무시하고 쿠킹이 비운다 | CurveData 14절 D2. 행 다형 저장보다 싸고 UE 모드 규칙을 그대로 따른다. 로드 뒤 불변식 판정 기반이 없고, 사람이 편집한 데이터로 에셋이 열리지 않으면 안 된다 |
| CAD4 | 색 커브 기본 키 | 생성자에 키를 넣지 않고, 편집기의 새로 만들기가 검정→흰색 키를 넣는다 | UE 편집기 생성자 키는 빌드 종류마다 클래스 기본값을 다르게 만든다(차이 저장 함정) |
| CAD5 | atlas 굽기 | 쿠킹 처리기가 Build 의존으로 굽는다. 원본 bulk는 저장하지 않는다 | 구독 없는 명령행 편집에서도 atlas가 낡지 않는다 |
| CAD6 | 임포터 기본 확장자 | 없음. `--importer=`로 고른다 | `csv`·`json`은 여러 종류가 쓴다. UE도 대화상자로 묻는다 |
| CAD7 | 원천 참조 | 원천은 `ObjectRef`(Hard) 참조 struct다. 소유 객체의 필드나 `StructRefArray` 원소에만 둔다. World는 `SoftObjectRef` + lease다. 값 struct에는 평탄 값을 둔다 | AssetSystem 6.2·6.3절, `ObjectRef` 복사 금지(`ObjectRefs.h:175-206`). UE `TObjectPtr`(하드)와 같은 로드·쿠킹 의미다 |
| CAD8 | 풀리지 않는 참조 | 오류로 돌려준다. 인라인으로 대신하지 않는다 | 조용히 다른 커브로 평가하면 데이터 문제가 숨는다 |
| CAD9 | 사본 | 사본(평가기·재생기·값 struct)은 참조를 풀어 평탄 값으로 둔다 | Registry 없는 스레드·사본에서도 평가할 수 있어야 한다 |
| CAD10 | 색 HSV | UE float 식을 `CurveAssets` 안에 따로 두고 Core는 바꾸지 않는다 | Core `LinearColor` HSV가 UE와 다르다(2.4.3절). Core 사용처에 영향을 주지 않는다 |
| CAD11 | CSV 규칙 | UE `FCsvParser`와 같은 관대한 규칙을 쓴다 | 같은 파일을 같은 칸으로 나눈다. 엄격한 RFC 4180은 UE에서 열리는 파일을 거부한다 |

### 7.2 UE와 다른 점

★는 결함 수정, ◇는 Rocks 규약이다.

1. ★ `CurveAsset` 시간·값 범위: 키 없는 채널은 합치지 않고, 키 있는 채널이 없으면 NotFound다. UE는 (0, 0)을 합쳐 범위에 0이 섞이고, 채널이 0개면 out을 건드리지 않는다.
2. ◇ 원천 평가는 풀리지 않는 참조를 오류로 돌려준다(CAD8). UE는 null 포인터일 때만 인라인을 쓴다(회수 개념이 없다).
3. ★ 표 행 추가에서 None 이름을 거부한다. UE `AddRichCurve`·`AddSimpleCurve`는 받지만, None 행은 `FindCurve`가 찾지 못한다.
4. ◇ `TryRenameRow`에서 원본 이름이 없으면 NotFound다. UE는 무시한다.
5. ★ 임포트에서 숫자가 아니거나 비유한인 칸은 실패다. UE `Atof`는 숫자가 아니면 0으로 읽는다(`CurveTable.cpp:623-635`, `CurveBase.cpp:162`).
6. ★ CSV 표의 시간 중복·행 부족·빈 문서는 실패이고, 기존 에셋을 지킨다. UE 팩터리는 먼저 표를 비우므로(`CSVImportFactory.cpp:354-360`) 빈 표가 남는다.
7. ★ 쿠킹 중복 키 제거는 사본에서 한다. UE는 저장 중에 원본 객체를 바꾼다. 그래서 편집기에서 쿠킹하면 편집 중인 에셋이 변형된다.
8. ◇ 표 모드가 다른 행에 접근하면 null이나 오류 결과다. UE는 `check`(중단)이거나 Error 로그다.
9. ◇ 조회·평가는 로그를 남기지 않고 호출자가 결과로 판단한다. UE `FindCurve`와 핸들 `Eval`은 경고 로그를 남긴다.
10. ◇ 표 변경 이벤트, 전역 캐시 번호, 쿠킹 중복 키 제거를 끄는 설정이 없다(미룸 — 1.2절, 5.3절).
11. ◇ 스칼라 채널 이름은 객체 이름이 아니라 "Value"다. Rocks 에셋 객체에는 이름이 없다.
12. ◇ 표 행 순서는 삽입 순서다. 지운 뒤 다시 넣거나 이름을 바꾸면 끝으로 간다. UE `TMap`(sparse set)은 지운 자리를 다시 쓴다.
13. ◇ 단일 커브 재임포트는 제자리 갱신이라 임포트하지 않은 필드(기본값·외삽·색 조정)를 지킨다. UE는 객체를 다시 만들 수 있다(확실치 않음).
14. ◇ JSON 한 객체 안의 중복 키는 문서 오류다. UE는 뒤의 값이 이긴다.
15. ◇ 쓰지 않는 모드의 맵에 행이 있어도 로드는 성공한다. 쿠킹이 그 맵을 비우고 경고를 남긴다. UE는 이런 상태가 생기지 않는다(맵이 하나다).

### 7.3 단계

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| CA1 | Runtime `CurveAssets`: `CurveAsset`과 클래스 셋, 색 조정(UE HSV), 평탄 값 struct, 원천 셋, 평가·사본 | 2장·3.1절 시험, 색 조정 UE 비트 대조 |
| CA2 | `CurveTableAsset`·`CurveTableRowRef` | 3.2·3.3절 시험 |
| CA3 | Core `CsvReader`, Developer `CurveAssetTools` 임포터 다섯, 진단 코드 `CurveImportRowSkipped`·`CurveTableUnusedRows`(에셋 세션 합의) | 5.1·5.2절 시험(UE 동작 대조표) |
| CA4 | `CurveTableCookProcessor` | 쿠킹 결과 키·접선, 에디터 객체 불변, 쓰지 않는 맵 비우기 |
| CA5 | InputActions 연결(6.1절 — 착수 때 방식 확정) | 입력 시험 갱신, slot 평탄화 시험 |
| CA6 | `ColorCurveAtlas`, 굽기, 쿠킹 처리기(4.3절 변경 목록 — 렌더·에셋 세션 합의) | 굽기 UE 대조, 쿠킹 텍스처 결과 |

- 단계마다 리뷰어를 하나 두고, 바뀐 시험과 format·policy를 돌린다.
- CA1이 push되면 6.2절의 세션들에 알린다.

### 7.4 시험

에셋 시스템 시험 원칙(대조표·조합·할당 실패 — AssetSystem 14장)을 따른다.

- **`CurveAsset_`**
  - 클래스 셋의 채널 수·이름·평가(키 없음, 기본값, 외삽)
  - 범위 합집합(빈 채널이 섞인 경우, 모두 빈 경우, 비유한)
  - `ResetCurve`가 기본값·외삽을 유지하는지
- **`ColorCurveAsset_`**
  - 색 조정 열한 단계를 UE 식과 비트로 대조한다. UE 원본 함수(HSV 변환 포함)를 시험 쪽에 float 그대로 옮겨 대조하고, CurveData 시험과 같은 방식을 쓴다.
  - 경계 입력:
    - 밝기 곡선 1 근처와 0
    - 활기 0 근처
    - LDR·HDR 판정 경계 1 + 1e-4
    - 음수 RGB(최댓값 ≤ 0 → (0, 0, 0, A))
    - 조정 뒤 S > 1(HSV → RGB가 자른다)
    - 알파 키 없음
  - 고정 판(`EvaluateClamped`)
  - Core HSV와 결과가 다른 반례(CAD10의 근거)
- **`CurveSource_`**
  - 인라인·참조 평가
  - 참조가 회수된 경우의 오류
  - 색: 참조는 조정 적용, 인라인은 원값
  - `TryFlattenInto` 실패 시 out 불변
  - 할당 실패
  - 원천 struct를 `Array` 원소·Map 값에 넣으면 메타 컴파일러가 거부하는지(RMC 진단 golden)
- **`CurveTable_`**
  - 모드 정하기, 다른 모드 추가 TypeMismatch, 같은 이름 대입의 자리 유지
  - 지우기·이름 바꾸기 순서(끝으로)
  - 이름 바꾸기 판정 순서 다섯
  - None 이름
  - `EmptyTable`
  - 평가 NotFound
  - 쓰지 않는 맵에 행이 있는 데이터(`asset-set`으로 저장)를 다시 열고 평가할 때 그 행이 무시되는지
  - 행 참조 셋 경우(null 표, 없는 행, 성공)
- **`Csv_`**(Core): UE `FCsvParser` 규칙 표
  - 빈 줄, 공백만 있는 줄
  - 따옴표 위치, `""`
  - 닫는 따옴표 뒤 글자, 닫히지 않은 따옴표
  - 줄 끝 셋, 끝 쉼표
  - 물리 줄 번호(빈 줄·따옴표 안 줄바꿈)
- **`CurveImport_`**
  - 임포터 다섯의 대조표: 정상, 행·항목 건너뜀 경고, 실패 사유마다
  - BOM(UTF-8·UTF-16 LE·BE), 잘못된 UTF-8은 CorruptData
  - 숫자 칸 trim, `inf`·`nan` 거부
  - JSON 값 종류(숫자·숫자 문자열·bool), 중복 키
  - 시간 열 0개
  - 재임포트 옵션 보존, 재임포트가 기본값·조정을 지키는지
  - 모드 결정(보간 셋), Cubic 키 접선의 UE 대조
  - 결정성(같은 원본 → 같은 byte)
- **`CurveTableCook_`**
  - 중복 키 제거 결과(Plain·Tangent)
  - Tangent의 키 3개 이상이면 접선 재계산, 미만이면 그대로
  - 에디터 객체 불변
  - 쓰지 않는 맵 비우기와 경고
  - 처리기 버전이 너겟 키에 드는지
- **`ColorCurveAtlas_`**(CA6)
  - 굽기 표본 시간(역수 곱, width 0·1 → 2)
  - null 커브 흰색
  - `disableAdjustments`(커브 원값, 텍스처 조정 항등)
  - float16 반올림
  - 커브 변경 → 너겟 키 변경
  - 처리기가 없는 쿠커에서 실패 진단

### 7.5 확인하지 못한 것

- UE에서 기본 키 그대로인 색 커브가 쿠킹 뒤 빈 커브가 되는지(2.4.4절). 설계에는 영향이 없다. Rocks는 생성자 키를 두지 않는다.
- UE 단일 커브 재임포트(`NewObject` 자리 재생성)가 임포트하지 않은 필드를 기본값으로 되돌리는지(7.2절 13).
- UE `UCompositeCurveTable`의 합성 규칙(미룸).
