# 커브 에셋 — 4. 색 커브 atlas

[목차](README.ko.md)

## 4. 색 커브 atlas(UE `UCurveLinearColorAtlas`)

머티리얼이 색 그라디언트를 텍스처 한 장에서 행 번호로 읽게 한다. 소비자는 머티리얼 노드 `Material.CurveAtlasRowParameter`(UE `UMaterialExpressionCurveAtlasRowParameter` — 머티리얼 세션 TODO MPQ-CurveAsset)다. 머티리얼은 atlas 에셋 참조와 행 번호로 번역한다.

### 4.1 UE 동작

- `UCurveLinearColorAtlas : UTexture2D`(`RE/Classes/Curves/CurveLinearColorAtlas.h:70-71`). 필드:
  - `TextureSize`(`uint32` 너비 — 생성자 256, 팩터리 64. 굽기·팩터리가 최소 2로 올린다 — `CurveLinearColorAtlas.cpp:75, 304`, `EditorFactories.cpp:2375`)
  - `GradientCurves`(`TArray<TObjectPtr<UCurveLinearColor>>`)
  - 편집기 전용 `bDisableAllAdjustments`(`:102-118`)
- 텍스처 설정(생성자 — `CurveLinearColorAtlas.cpp:14-34`): mip 없음, Bilinear, `SRGB = false`, 주소 Clamp·Clamp, 압축 `TC_HDR`.
- 굽기(`UpdateTextures` — 편집기, `:300-344`):
  - 원본은 RGBA16F이고 크기는 너비 × max(커브 수, 1)이다.
  - 행 i는 커브 i를 시간 x × (1 / (W − 1))에서 표본한 값이다(`PushToSourceData` — `CurveLinearColor.cpp:245-248`, `check(Width > 1)`. 조정 적용, `bDisableAllAdjustments`면 원값).
  - 커브가 null인 행은 흰색이다.
  - 굽고 나면 일반 텍스처 빌드를 거친다.
- 굽는 시점: 편집기 PostLoad에서 커브 변경을 구독해 매번 다시 굽는다(`:210-297`).
- `GetCurvePosition`·`GetCurveIndex`는 정규화하지 않은 행 번호를 돌려준다(`:354-374`). 머티리얼 노드가 UV를 계산한다(`MaterialExpressions.cpp:20862-20900`).

### 4.2 Rocks 모양

- `class ColorCurveAtlas : public Texture2D`(Runtime `CurveAtlas` 모듈 — 1.4절). UE처럼 텍스처 자체다. 그래야 머티리얼·렌더가 일반 텍스처로 다룬다. `Texture2D`는 지금 `final`이라 그 표지를 뗀다(Texture 모듈 변경 — 렌더·에셋 세션에 알림).
- 필드:
  - `ROCKS_PROPERTY() uint32 width = DefaultColorCurveAtlasWidth;`(이름 있는 상수 256 — UE 생성자 값). 굽기는 max(width, `MinColorCurveAtlasWidth` = 2)를 쓴다(UE와 같다)
  - `ROCKS_PROPERTY() ObjectRefArray<ColorCurveAsset> curves;`(Hard)
  - `ROCKS_PROPERTY() bool disableAdjustments = false;`
  - UE의 `bDisableAllAdjustments`는 편집기 전용이지만, Rocks는 쿠킹 때 굽기 때문에 쿠커가 읽어야 한다. 그래서 EditorOnly가 아니다. 쿠킹 데이터에서는 빼도 된다(값이 쿠킹 결과에 이미 반영된다). 빼는 방법은 CA6 때 쿠킹 처리기에서 정한다.
- 텍스처 설정의 기본값은 UE 생성자 값과 같다. mip 없음, 선형, sRGB 아님, Clamp, HDR 형식(Rocks 텍스처 설정 이름으로 옮긴다 — AssetSystem 12장).
- `disableAdjustments`(UE `bDisableAllAdjustments`): 켜면 커브 조정 대신 원값으로 굽고, 텍스처 자체 조정값(`Texture`의 `adjust*` 필드 — `Texture.h:37-44`)도 항등값으로 보고 빌드한다. UE는 켤 때 텍스처 조정값을 항등으로 되돌리고(`CurveLinearColorAtlas.cpp:164-189`) 편집을 막는다(`CanEditChange` — `:36-61`). Rocks 쿠킹 처리기는 켜진 동안 텍스처 조정값을 항등으로 바꾼 사본으로 빌드한다(편집 차단은 편집기 일).
- `usize RowOf(ObjectHandle curve) const`: 행 번호를 돌려준다. 없으면 `IndexNone`이다(UE `GetCurveIndex`).

### 4.3 굽기 시점(CAD5)

- Rocks에는 아직 편집기 구독(Studio)이 없다. 그래서 **쿠킹 처리기가 굽는다**.
  - 처리기는 `curves`를 Build 의존으로 선언한다(AssetSystem 16.2절 — 커브가 바뀌면 너겟 키가 바뀐다).
  - 원본(RGBA16F, 너비 × max(커브 수, 1))을 UE 식대로 만든다.
  - 그 원본으로 `Texture2D` 빌드 경로(TextureTools)를 부른다.
- 에디터 패키지에는 원본 bulk를 저장하지 않는다(굽기는 결정적이다). 편집기 미리보기는 같은 굽기 함수를 메모리에서 부른다.
- 버린 대안:
  - 편집 때 굽어 원본 bulk 저장(UE 방식 — `UpdateTextures`, `CurveLinearColorAtlas.cpp:300-317`): 구독 없는 명령행 편집(asset-set)이 커브를 바꾸면 atlas가 낡는다. UE는 편집기 구독(`OnUpdateCurve`)으로 이 문제를 피한다. UE와 다른 점으로 7.2절 22에 적는다. Studio 커브 편집기가 구독으로 다시 굽게 되면 다시 본다.
- Texture 쪽 변경(final 해제·`CookDimensionOf`·메모리 원본 입력)은 에셋 세션(rocks-5f) 몫이다(렌더 세션 확인 2026-11-27 — 렌더 업로드는 플랫폼 데이터만 보고 에셋 클래스를 보지 않아 영향이 없다).
  - atlas를 텍스처가 아닌 에셋으로 두고 쿠킹이 별도 Texture2D를 생성: 머티리얼 참조가 둘로 갈린다.
- 굽기 함수 `BakeColorCurveAtlas(...)`(Developer `CurveAtlasTools`): 입력은 atlas와 풀린 커브 목록이고, 출력은 RGBA16F 픽셀 배열이다.
  - 표본 시간은 float x × (1 / (W − 1))이다(UE 식 그대로 — 나눗셈 한 번이 아니라 역수 곱이라 반올림이 다르다). W는 위의 max(width, 2)다.
  - 조정 평가의 `powf`는 플랫폼 libm에 따라 비트가 다를 수 있다. 쿠킹 byte 결정성은 CurveData D17과 같은 범위(Windows x64 쿠커 기준)로 보장하고, 다른 호스트는 허용오차 대조다.
  - float16 변환은 Core half 변환(UE `FFloat16Color`와 같은 반올림)이다.
- CA6 변경 목록(다른 세션 파일 — 렌더·에셋 세션 합의 뒤):
  - `Texture2D`의 `final` 해제(Texture 모듈).
  - `TextureCook.cpp`의 `CookDimensionOf`(정확한 TypeId 비교 — `:67`)가 파생 타입을 받게.
  - Texture 쿠킹 처리기의 원본 입력 경로(bulk 대신 메모리 원본). 없으면 그 API를 보강한다(우회하지 않는다).
  - `CookProcessorTable::Find`는 가장 가까운 등록 처리기라(`CookProcessor.h:108-111`), `CurveAtlasTools`가 등록되지 않은 쿠커에서는 atlas가 `Texture2D` 처리기로 가서 없는 원본 bulk를 읽는다. 처리기는 원본 bulk가 없으면 `MissingDependency`류 진단으로 실패한다(조용한 빈 텍스처 금지).

### 4.4 범위

- 이번 문서에서 정하는 것은 위 모양과 굽기 시점이다. 구현은 마지막 단계 CA6이다. 머티리얼 쪽 번역(행 번호 → V 좌표)은 머티리얼 세션이 한다. UE 식 `(index + 0.5) / height`는 UE 노드 코드를 따른다.
- UE 편집기의 조정값 캐시·복원(`:98-153`)은 편집기 일이라 두지 않는다.
