# 커브 에셋 — 1. 요약·범위·대응표·모듈

[목차](README.ko.md)

## 1. 요약·범위·대응표·모듈

### 1.1 요약

- 커브를 에셋으로 저장하고 여러 곳(타임라인·진동 효과·소리 감쇠·후처리 노출·게임 수치 표)이 함께 쓴다. 값 타입(`TangentCurve`·`PlainCurve`)은 이미 있고 이 문서는 그것을 감싸는 에셋 클래스와 참조 방식을 정한다.
- 에셋 클래스는 리플렉션 클래스(`Asset` 파생 — AssetSystem 6장)다. 쿠킹은 기본 처리기다(빌드할 파생 데이터가 없다). 예외는 커브 표의 쿠킹 때 중복 키 제거(UE와 같다 — 5.3절)와 atlas(텍스처 — 4장)다.
- 평가 의미는 UE와 같다. 결함은 고치고 7.2절에 적는다.

### 1.2 범위

| 들인다 | 미룬다(이유) | 들이지 않는다(이유) |
|---|---|---|
| 스칼라·벡터·색 커브 에셋, 색 조정, 인라인·에셋 원천 셋, 커브 표(Plain·Tangent 모드), 표 행 참조, 색 커브 atlas, CSV·JSON 표 임포트, CSV 단일 커브 임포트, 쿠킹 중복 키 제거, 진동·입력 응답 커브 연결 | 편집기(커브 편집기·표 편집기·썸네일 — Studio), 표 내보내기(CSV·JSON — 도구가 필요해질 때, UE JSON 내보내기 결함은 7.2절), 합성 표(`UCompositeCurveTable` — 요구 시), 표 변경 이벤트(`OnCurveTableChanged` — 편집기·캐시 소비자가 생길 때), 타임라인 컴포넌트(CM5) | `.as` 진폭 커브 임포트(UE 실험 기능 — `EditorFactories.cpp:7239-7329`), 리플렉션 JSON 전체 덮어쓰기(`UCurveBase::ImportFromJSONString` — Rocks 리플렉션 텍스트·asset-set이 같은 일을 한다), 핫픽스 경로(네트워크 단계) |

### 1.3 대응표

Rocks 이름은 서술형이다(값 커브 `TangentCurve`와 헷갈리지 않게 에셋 클래스에 `Asset`을 붙인다 — CAD1). UE 이름은 이 표에만 둔다.

| UE 5.8 | Rocks | 한 줄 설명 |
|---|---|---|
| `UCurveBase`(`RE/Classes/Curves/CurveBase.h:16`) | `CurveAsset` | 커브 에셋 추상 기반: 채널 목록·시간·값 범위·초기화 |
| `UCurveFloat`(`CurveFloat.h:29`) | `FloatCurveAsset` | `TangentCurve` 하나 + 이벤트 커브 표지 |
| `UCurveVector`(`CurveVector.h:29`) | `VectorCurveAsset` | `TangentCurve` X·Y·Z |
| `UCurveLinearColor`(`CurveLinearColor.h:28`) | `ColorCurveAsset` | `TangentCurve` R·G·B·A + 색 조정 일곱 |
| `FRuntimeFloatCurve`·`FRuntimeVectorCurve`·`FRuntimeCurveLinearColor` | `FloatCurveSource`·`VectorCurveSource`·`ColorCurveSource` | 인라인 커브 + 에셋 참조(참조가 있으면 에셋 우선). 참조 struct라 객체 필드·`StructRefArray`에만 둔다 |
| (없음 — UE는 `FRichCurve` 사본) | `VectorCurveValue`·`ColorCurveValue` | 참조 없는 평탄 값(사본·값 struct용 — 2.5절) |
| `UCurveTable`(`RE/Classes/Engine/CurveTable.h:39`) | `CurveTableAsset` | 이름 → 스칼라 커브 표(Plain·Tangent 모드 하나) |
| `ECurveTableMode` | `CurveTableMode` | Empty 0·Plain 1·Tangent 2(UE SimpleCurves·RichCurves) |
| `FCurveTableRowHandle`(`CurveTable.h:261`) | `CurveTableRowRef` | 표 참조 + 행 이름 |
| `EEvaluateCurveTableResult` | (없음) | `Result<float>`의 NotFound가 대신한다 |
| `UCurveLinearColorAtlas`(`CurveLinearColorAtlas.h:70`) | `ColorCurveAtlas` | 색 커브 여러 개를 한 줄씩 구운 `Texture2D` |
| `UCSVImportFactory`·`UReimportCurveTableFactory`·`UReimportCurveFactory` | `curve_table_csv`·`curve_table_json`·`curve_csv` 임포터 | A2f 임포터(5.1절) |
| `FCsvParser`(`Runtime/Core/Private/Serialization/Csv/CsvParser.cpp`) | Core `CsvReader` | UE와 같은 관대한 CSV 읽기(5.2절) |

### 1.4 모듈

- **Runtime `CurveAssets`**(`Engine/Source/Runtime/CurveAssets`, 의존 Core·ObjectRuntime·Curves·AssetRuntime): 2·3장의 클래스와 struct. Engine 층이고 Core 층이 아니다 — `Asset`(AssetRuntime)에 기대기 때문이다. 소비자(InputActions·Audio·Material·게임 모듈)가 이 모듈을 의존한다.
- **Runtime `Texture`에 기대는 atlas**: `ColorCurveAtlas`는 `Texture2D` 파생이라 `CurveAssets`가 `Texture`를 의존하면 모든 커브 소비자가 텍스처 모듈을 끌고 온다. 그래서 atlas는 따로 Runtime `CurveAtlas` 모듈(의존 CurveAssets·Texture)에 둔다(CAD2).
- **Developer `CurveAssetTools`**(`Engine/Source/Developer/CurveAssetTools`, 의존 CurveAssets·AssetDatabase·AssetCooker): 임포터 셋·커브 표 쿠킹 처리기. atlas 굽기는 Developer `CurveAtlasTools`(의존 CurveAtlas·CurveAssets·Texture·AssetRuntime — 굽기 함수와 타입 hook 셋: 원본 재구성·Build 의존·정규화, 4장). atlas 쿠킹은 텍스처 처리기다.
- **Core `CsvReader`**(`Core/Text/Csv.h`): 범용 기능이라 Core에 둔다(공용 기능은 기능 모듈에 자체 구현하지 않는다). 데이터 표(DataTable 대응)도 같은 것을 쓴다.
- 한 파일에 한 클래스, 범주 폴더(`CurveAssets/` 아래 `FloatCurveAsset.h` 등)다.
