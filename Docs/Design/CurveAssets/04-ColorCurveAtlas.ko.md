# 커브 에셋 — 4. 색 커브 atlas

[목차](README.ko.md)

## 4. 색 커브 atlas(UE `UCurveLinearColorAtlas`)

머티리얼이 색 그라디언트를 텍스처 한 장에서 행 번호로 읽게 한다. 소비자는 머티리얼 노드 `Material.CurveAtlasRowParameter`(UE `UMaterialExpressionCurveAtlasRowParameter` — 머티리얼 세션 TODO MPQ-CurveAsset)다. 머티리얼은 atlas 에셋 참조와 행 번호로 번역한다.

### 4.1 UE 동작

- `UCurveLinearColorAtlas : UTexture2D`(`RE/Classes/Curves/CurveLinearColorAtlas.h:70-71`). 필드:
  - `TextureSize`(`uint32` 너비 — 생성자 256, 팩터리 64. 굽기·편집이 최소 2로 올려 필드에 다시 쓴다 — `CurveLinearColorAtlas.cpp:75, 304`, `EditorFactories.cpp:2375`)
  - `GradientCurves`(`TArray<TObjectPtr<UCurveLinearColor>>` — 런타임 필드, 쿠킹 패키지도 커브를 참조한다)
  - 편집기 전용 `bDisableAllAdjustments`(`:102-118`)
- 텍스처 설정(생성자 — `CurveLinearColorAtlas.cpp:14-34`): mip 없음, Bilinear, `SRGB = false`, 주소 Clamp·Clamp, 압축 `TC_HDR`.
- 굽기(`UpdateTextures` — 편집기, `:300-344`):
  - 원본은 RGBA16F이고 크기는 너비 × max(커브 수, 1)이다(`Source.Init(TSF_RGBA16F)` — 원본에 구워 패키지에 저장된다).
  - 행 i는 커브 i를 시간 x × (1 / (W − 1))에서 표본한 값이다(`PushToSourceData` — `CurveLinearColor.cpp:245-248`, `check(Width > 1)`. 조정 적용, `bDisableAllAdjustments`면 원값).
  - 커브가 null인 행은 흰색이다. 커브가 없으면 한 줄 흰색이다.
  - 굽고 나면 `PostEditChange`로 일반 텍스처 빌드를 거친다. 쿠킹은 보통 `UTexture2D` 경로다.
- 굽는 시점:
  - 편집: `TextureSize`·`GradientCurves`·`bDisableAllAdjustments`를 바꾸면 다시 굽는다(`PostEditChangeProperty` — `:63-161`).
  - 로드: `PostLoad`가 커브 변경을 구독하고 늘 다시 굽는다(`:210-235` — "코드가 안정되면 nop", `:224`). 쿠커가 atlas를 로드할 때도 그때의 커브로 다시 구워진다. 이것이 UE 쿠킹 정확성을 지킨다.
  - 커브 편집: 구독(`OnCurveUpdated` — `:264-296`)이 열린 동안 즉시 다시 굽는다.
- `bDisableAllAdjustments`를 켜면 텍스처 조정값을 캐시하고 항등값(밝기 1, 밝기 커브 1, 생동 0, 채도 1, RGB 커브 1, 색조 0, 알파 0–1, 크로마 키 끔)으로 되돌린다(`CacheAndResetColorAdjustments` — `:164-189`). 끄면 캐시를 되돌린다. 켜진 동안 편집기는 조정 필드를 막는다(`CanEditChange` — `:36-61`).
- `GetCurvePosition`·`GetCurveIndex`는 정규화하지 않은 행 번호를 돌려준다(`:354-374`). 머티리얼 노드가 UV를 계산한다(`MaterialExpressions.cpp:20862-20900`).

### 4.2 Rocks 모양

- `class ColorCurveAtlas : public Texture2D`(Runtime `CurveAtlas` 모듈 — 1.4절). UE처럼 텍스처 자체다. 그래야 머티리얼·렌더·쿠커가 일반 텍스처로 다룬다. `Texture2D`는 `final`을 뗀다(Texture 모듈 — 렌더·에셋 세션 합의 2026-11-27).
- 필드:
  - `ROCKS_PROPERTY() uint32 width = DefaultColorCurveAtlasWidth;`(이름 있는 상수 256 — UE 생성자 값). 굽기는 max(width, `MinColorCurveAtlasWidth` = 2)를 쓰고, 정규화 hook이 필드도 그 값으로 올린다(UE와 같다).
  - `ROCKS_PROPERTY() ObjectRefArray<ColorCurveAsset> curves;`(Hard — UE처럼 런타임 필드다. null 원소는 흰 행이다)
  - `ROCKS_PROPERTY(EditorOnly) bool disableAdjustments = false;`(UE `bDisableAllAdjustments` — UE처럼 편집기 전용. 굽기가 원본에 반영하므로 쿠커는 읽지 않는다)
- 텍스처 설정 기본값은 UE 생성자 값이다. 생성자가 `mipGenSettings = NoMipmaps`, `filter = Bilinear`, `srgb = 0`, `addressX·Y = Clamp`, `compressionSettings = HDR`로 둔다.
- 정규화 hook(AssetSystem 12.12절 — base인 Texture 정규화를 먼저 부른다):
  - `width = max(width, 2)`.
  - `disableAdjustments`가 켜져 있으면 텍스처 조정값을 UE 항등값으로 두고 `chromaKeyTexture = false`다(UE `CacheAndResetColorAdjustments`). 켜진 동안 조정 필드를 대입해도 정규화가 다시 항등값으로 되돌린다.
  - UE 캐시·복원(끌 때 이전 조정값으로 되돌리기)은 두지 않는다(편집기 일 — 4.4절).
- `usize RowOf(ObjectHandle curve) const`: 행 번호를 돌려준다. 없으면 `IndexNone`이다(UE `GetCurveIndex`).

### 4.3 굽기 시점(CAD5 — UE 방식)

UE처럼 **atlas가 자기 `TextureSource`에 구워 저장한다**. 쿠커에게 atlas는 보통 `Texture2D`이고, 텍스처 쿠킹 처리기가 그 원본을 빌드한다. 굽기는 원본 재구성 hook(AssetSystem 12.14절 A5j2, `AssetSourceRebuildHook`)이 한다.

- 부르는 때(sink `Cause()`):
  - `Edit`(asset-set)·`New`(asset-new): UE `PostEditChangeProperty` 대응.
  - `Open`(에디터 패키지를 열 때 — AssetSystem 12.14절 A5j2c): UE `PostLoad` 대응이다. 쿠커도 `EditorPackageSet::Open`으로 열므로, 커브를 asset-set으로 바꾼 뒤 atlas 원본이 낡아 있어도 쿠킹은 그때 커브로 다시 구운 원본을 쓴다.
  - `Propagate`(텍스처 원소 재임포트 전파): 아무것도 하지 않는다(atlas 원본은 텍스처를 묶지 않는다).
- hook은 assignedFields를 보지 않는다. 늘 굽고, 지금 원본과 비교한다. 같으면 아무것도 쓰지 않고, 다르면 `AddBulk` → `source` 교체 → `MarkRebuilt`다. 비교는 형식·크기·slice·mip·layer와 bulk 내용(`BulkDataRef::SameContent` — raw 길이·SHA-256)이다. 그래서 낡지 않은 atlas를 열어도 dirty가 되지 않는다. UE는 로드마다 다시 그린다(같은 값이면 결과도 같다).
- 커브 변경의 즉시 전파(UE `OnCurveUpdated` — 커브를 asset-set으로 바꾸면 그 커브를 쓰는 atlas를 다시 구워 저장)는 지금 없다. 낡은 저장 원본은 다음 열기 때 다시 구워진다. 즉시 전파가 필요하면 에셋 세션에 요구로 적는다(rocks-5f 2026-11-27 — 정확성에는 필요 없다).
- **Build 의존**: atlas의 Build 의존 hook이 `curves`의 각 패키지를 Build 의존으로 더한다(AssetSystem 16.2절). 저장된 atlas 원본이 낡아 있어도 커브 내용이 쿠킹 키에 들어가 너겟이 무효가 된다. 텍스처 처리기의 너겟 키는 처리기 입력 원본의 SHA-256(`TextureSource::bulk.sha256`)을 쓰는데, Open에서 다시 구운 저장 전 원본이면 그 값은 새 bulk의 SHA-256이다(`AddUnsaved`).
- 굽기 함수 `BakeColorCurveAtlas(...)`(Developer `CurveAtlasTools`): 입력은 풀린 커브 목록(null 허용)·너비·`disableAdjustments`이고, 출력은 RGBA16F 픽셀 byte(행 우선, 성분 r g b a, little-endian)다.
  - 크기: 너비 W = max(width, 2), 높이 = max(커브 수, 1). 둘 중 하나가 `MaxColorCurveAtlasDimension`(16384 — 텍스처 원본 임포트 상한 `ImageDecodeLimits::DefaultMaxDimension`과 같다)을 넘으면 `InvalidSetting`(detail = 넘은 값)이다. UE는 상한 없이 굽고 빌드가 최대 크기로 줄인다(AssetSystem 12.5절 4번) — 7.2절 22.
  - 표본 시간은 float x × (1 / (W − 1))이다(UE 식 그대로 — 나눗셈 한 번이 아니라 역수 곱이라 반올림이 다르다).
  - 값은 `ColorCurveAsset`의 조정 평가(2.3절, `disableAdjustments`면 원값)이고 null 커브는 흰색 (1, 1, 1, 1)이다.
  - 조정 평가의 `powf`는 플랫폼 libm에 따라 비트가 다를 수 있다. 원본 byte 결정성은 CurveData D17과 같은 범위(Windows x64 기준)로 보장하고, 다른 호스트는 허용오차 대조다. 호스트가 달라 원본 byte가 다르면 Open이 dirty로 표시할 수 있다(저장하면 그 호스트 값이 된다).
  - float16 변환은 Core `Float16::FromFloat`(round-to-nearest-even — UE `FFloat16Color`의 `FFloat16::Set`과 같은 규칙)다.
- 쿠커가 연 패키지가 dirty가 되어도 쿠킹은 에디터 패키지를 저장하지 않는다. 메모리 원본만 쓴다(CA6 시험이 본다).
- Texture 쪽 변경(CA6 — 에셋 세션 합의, 이 세션이 작게 한다):
  - `Texture2D`의 `final` 해제.
  - `TextureCook.cpp`의 `CookDimensionOf`(정확한 TypeId 비교)가 파생 타입을 받게 한다(`IsA` — 정규화의 차원 판정 `TextureSettingsNormalize.cpp`와 같다).
- 버린 대안:
  - 쿠킹 처리기가 메모리에서 굽고 원본을 저장하지 않음(개정 2까지의 안): Texture 쿠킹에 메모리 원본 입력 경로가 따로 필요하고, 에디터 미리보기·렌더 업로드도 원본 없이 다뤄야 한다. UE와 다르고 얻는 것은 패키지 크기뿐이다.
  - atlas를 텍스처가 아닌 에셋으로 두고 쿠킹이 별도 Texture2D를 생성: 머티리얼 참조가 둘로 갈린다.

### 4.4 범위

- 구현은 마지막 단계 CA6이다. 머티리얼 쪽 번역(행 번호 → V 좌표)은 머티리얼 세션이 한다. UE 식 `(index + 0.5) / height`는 UE 노드 코드를 따른다.
- UE 편집기의 조정값 캐시·복원(`:164-208`)과 조정 필드 편집 차단(`CanEditChange`)은 편집기 일이라 두지 않는다. 정규화가 켜진 동안 항등값을 지킨다.
