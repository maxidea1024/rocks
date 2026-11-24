## 4. 글꼴 에셋

필드 이름은 camelCase이고 bool에 `b` 접두를 쓰지 않는다(전체 규칙). enum 값 이름은 업계 공용어(Default·Auto·Bitmap·Msdf 등)라 UE와 같게 둔다(UiToolkit UD12 (1)). 기본값은 UE 생성자·enum 0번과 같다. 예외는 `Font::cacheType`(UE enum 0번은 Offline이지만 새 Font를 만드는 `UFontFactory`가 Runtime으로 둔다 — `EF` 5124행, Rocks는 이 값을 기본값으로 둔다) 하나다.

### 4.1 열거형

| Rocks | 값(순서 = UE) | UE |
|---|---|---|
| `FontHinting` | Default·Auto·AutoLight·Monochrome·None | `EFontHinting` |
| `FontLoadingPolicy` | LazyLoad·Stream·Inline | `EFontLoadingPolicy` |
| `FontLayoutMethod` | Metrics·BoundingBox | `EFontLayoutMethod` |
| `FontRasterizationMode` | Bitmap·Msdf·Sdf·SdfApproximation | `EFontRasterizationMode` |
| `FontCacheType` | **Prebaked**·**OnDemand** | `EFontCacheType` Offline·Runtime(FD4) |
| `FontRuntimeSource` | Asset·DefaultStyle | `ERuntimeFontSource` Asset·CoreStyleDefault |
| `FontFallback` | None·LastResort·All | `EFontFallback` FF_NoFallback·FF_LastResortFallback·FF_Max |
| `TextShapingMethod` | Auto·KerningOnly·FullShaping | `ETextShapingMethod` |

TextLayout은 Object를 모르므로 열거형 본체는 TextLayout의 일반 enum이고, `Font` 모듈이 같은 값의 리플렉션 enum 미러를 둔다(미러와 본체의 값 일치는 `static_assert`로 묶는다).

### 4.2 `FontFace : Asset`

원본 글꼴 파일 하나(TTC·OTC면 하위 face 여러 개를 담은 파일 하나).

| 필드 | 형식·기본값 | 의미 | UE |
|---|---|---|---|
| `hinting` | `FontHinting` = Default | hinting 방식(6.2절 대응표) | `Hinting` |
| `loadingPolicy` | `FontLoadingPolicy` = LazyLoad | 실행 중 원본 byte를 어떻게 올리나(4.4·4.5절) | `LoadingPolicy` |
| `layoutMethod` | `FontLayoutMethod` = Metrics | 줄 높이를 글꼴 metric과 경계 상자 중 무엇으로 | `LayoutMethod` |
| `overrideAscent` · `ascentOverride` | bool = false · int32 = 0(−100–100) | ascent 재정의(글꼴 단위) | `bIsAscendOverridden` · `AscendOverriddenValue` |
| `overrideDescent` · `descentOverride` | bool = false · int32 = 0(−100–100) | descent 재정의 | `bIsDescendOverridden` · `DescendOverriddenValue` |
| `strikeHeightPercent` | int32 = 60(0–100) | 취소선 높이(글꼴 높이 대비) | `StrikeBrushHeightPercentage` |
| `distanceField` | `FontDistanceFieldSettings` | 거리장 래스터 설정(아래) | `bEnableDistanceFieldRendering` 외 7개 |
| `sourceData` | `BulkDataRef` | 원본 파일 byte 그대로(SHA-256). 쿠킹 배치는 4.4절 | `FontFaceData->Data` |
| `subFaceCount` | int32(Transient) | 하위 face 수 — 로드 뒤 backend로 다시 센다 | `SubFaces`(transient) |

하위 face의 family·style 이름 목록은 저장하지 않는다. UE와 같이 transient로 두고 편집기가 열 때 backend로 다시 읽는다(낡은 값이 남지 않게). 저장 검증(`subFaceIndex` 범위)도 그때 계산한 값으로 한다.

`FontDistanceFieldSettings`(`ROCKS_STRUCT`):

| 필드 | 기본값 | UE |
|---|---|---|
| `enabled` | false | `bEnableDistanceFieldRendering` |
| `sdfPpem` | {low 32, medium 48, high 64}(8–256) | `Min/Mid/MaxDistanceFieldPpem` |
| `msdfPpem` | {low 32, medium 40, high 56}(8–256) | `Min/Mid/MaxMultiDistanceFieldPpem` |
| `overrideModes` · `modeOverride` | false · {msdf→Msdf, sdf→Sdf, sdfApproximation→SdfApproximation} | `PlatformRasterizationModeOverrides`(`TOptional`) |

- **원본 파일 이름 필드가 없다.** UE `SourceFilename`은 편집기 원본 경로와 쿠킹된 `.ufont` 경로를 겸한다. Rocks는 원본 경로·SHA-256을 ImportRecord가, 쿠킹된 byte 위치를 쿠킹 데이터(4.4절)가 가진다(FD5).
- 원본 byte는 **디코딩하지 않고 파일 그대로** 저장한다. 텍스처(정규 픽셀로 풀어 저장 — 에셋 시스템 12.1절)와 다른 이유: FreeType·HarfBuzz가 sfnt 표를 직접 읽으며, 변환할 정규형이 없다(FD3).
- 편집 후(`PostEditChange` 대응): TextLayout 글꼴 cache에서 이 face를 쓰는 항목을 비운다(UE `FlushFontCache`, `FFC` 224·235행). 편집 문맥만이다.

### 4.3 `Font : Asset`

| 필드 | 형식·기본값 | 의미 | UE |
|---|---|---|---|
| `cacheType` | `FontCacheType` = OnDemand(머리말의 예외) | 4.1절 | `FontCacheType` |
| `runtimeSource` | `FontRuntimeSource` = Asset | DefaultStyle이면 엔진 기본 조합 글꼴(7.5절)을 쓴다 | `RuntimeFontSource` |
| `compositeFont` | `CompositeFontDesc`(OnDemand) | 저장 조합 기술(4.3.1절) | `CompositeFont` |
| `defaultSize` | int32 = 9(1–1000) | 크기를 안 주는 Canvas 호출의 크기 | `LegacyFontSize` |
| `defaultTypeface` | `Name` | typeface를 안 주는 Canvas 호출의 typeface | `LegacyFontName` |
| `prebaked` | `PrebakedFontData`(Prebaked) | 4.3.2절 | `Characters`·`Textures`·`CharRemap` 외 |
| `scalingFactor` | float = 1.0 | 미리 구운 글꼴 배율 | `ScalingFactor` |
| `importOptions` | `PrebakedFontImportOptions`(EditorOnly) | 미리 구운 글꼴 재임포트 설정(5.2절) | `ImportOptions` |

#### 4.3.1 저장 조합 기술(`Font` 모듈 `ROCKS_STRUCT`)

```text
CompositeFontDesc   { TypefaceDesc defaultTypeface; FallbackFontDesc fallbackTypeface;
                      Array<SubFontDesc> subTypefaces; bool enableAscentDescentOverride = true; }
TypefaceDesc        { Array<TypefaceEntryDesc> entries; }
TypefaceEntryDesc   { Name name; ObjectRef<FontFace> face; int32 subFaceIndex = 0; }
FallbackFontDesc    { TypefaceDesc typeface; float scalingFactor = 1.0; }
SubFontDesc : FallbackFontDesc { Array<CodePointRange> characterRanges; Array<Name> cultures;
                      String editorName(EditorOnly); }
CodePointRange      { uint32 first; uint32 last; }     // 닫힌 구간, first ≤ last ≤ 0x10FFFF
```

- face 참조는 Hard 참조다(UE `FontFaceAsset`이 `TObjectPtr`). Font를 로드하면 face 객체도 로드된다. 원본 byte를 언제 올리는지는 face의 `loadingPolicy`가 정한다.
- UE `FFontData`의 파일 이름 경로(`FontFilename` — face 에셋 없이 디스크 파일을 가리킴)는 두지 않는다(FD6, 예외는 7.5절 내장 글꼴).
- UE `FCompositeSubFont::Cultures`는 `;`로 이은 문자열이다. Rocks는 `Array<Name>`이다(검증·중복 거부를 저장 때 한다).
- 편집기의 구간 선택은 이름 있는 유니코드 블록 표(UE `FUnicodeBlockRange` 대응 `UnicodeBlockRanges` — TextLayout 데이터 표)를 쓴다. 저장 형식은 `CodePointRange`뿐이다.
- 검증(저장 거부): 같은 typeface 안 entry 이름 중복, `subFaceIndex` ≥ face의 하위 face 수, 구간 범위 밖·역전, 빈 기본 typeface(OnDemand이고 `runtimeSource` = Asset일 때).

#### 4.3.2 미리 구운 글꼴 데이터(`PrebakedFontData`)

| 필드 | 의미 | UE |
|---|---|---|
| `characters: Array<FontCharacter>` | 문자마다 `{ startU, startV, sizeU, sizeV, pageIndex(uint8), verticalOffset }` | `FFontCharacter` |
| `pages: ObjectRefArray<Texture2D>` | 페이지 텍스처. **별도 Texture2D 패키지**(아래) | `Textures`(inner) |
| `codePointMap: Map<uint32, uint16>` | code point → `characters` 번호. 비어 있으면 code point가 곧 번호 | `CharRemap`·`IsRemapped` |
| `emScale`·`ascent`·`descent`·`leading` | metric | 같은 이름 |
| `kerning` | int32, 문자 사이 기본 간격 | `Kerning` |

- **페이지는 별도 패키지다.** UE는 페이지 `UTexture2D`를 UFont의 하위 객체로 둔다. Rocks 에셋 규칙은 "Asset의 Outer는 Package, 패키지당 대표 에셋 하나"라([에셋 시스템](../AssetSystem/06-AssetTypes.ko.md) 6.1절, 4장) Texture2D를 Font 안에 둘 수 없다. 그래서 생성기(5.2절)가 Font 옆에 `<Font이름>_Page<N>` Texture2D 패키지를 만들고, 그 ImportRecord의 소유자(`ownerAssetId`)를 Font로 둔다(에셋 시스템 6.2절 ImportOwner — 다중 출력 임포트의 소유 패키지). Font가 페이지를 Hard로 참조한다. 페이지 텍스처는 텍스처 파이프라인(압축 설정·LOD 그룹·쿠킹)을 그대로 탄다(FD18).
- UE `CharRemap`은 `uint16` 키(UTF-16 한 단위)다. Rocks는 **code point(`uint32`) 키**라 보조 평면 문자도 굽는다(FD7).
- 없는 문자는 UE `NULLCHARACTER`(127)처럼 대체 문자(U+007F가 표에 있으면 그것, 없으면 그리지 않음)로 그린다.
- 로드 뒤 계산(transient): 문자 수, 최대 문자 높이(UE `CacheCharacterCountAndMaxCharHeight`).

### 4.4 쿠킹 데이터와 저장 위치

| `loadingPolicy` | 원본 byte·전처리 geometry 위치(쿠킹) | 로드 때 |
|---|---|---|
| Inline | 패키지 일반 bulk entry | 패키지와 함께 올라온다 |
| LazyLoad | 패키지의 **지연 bulk entry**(패키지 로드가 읽지 않는 entry — 선행 FP1) | 처음 쓸 때 entry 전체를 읽는다. 기본은 동기, `ui.font.async_lazy_load`면 비동기(4.5절) |
| Stream | 지연 bulk entry, **원본 byte는 비압축**(임의 위치 읽기) | face를 열 때 범위 읽기 handle을 만들고 FreeType 스트림 콜백이 필요한 범위만 읽는다 |

- UE는 Inline이 아니면 패키지 옆 `.ufont` 파일을 따로 쓴다(1.2절). Rocks는 같은 패키지 안의 지연 bulk entry로 둔다(FD8). 텍스처 mip 스트리밍도 "덩어리가 패키지 안인지 별도 파일인지"를 AssetSystem이 정하기로 했다([텍스처 스트리밍](../TextureStreaming.ko.md) 5.4절). 글꼴은 그 결정을 그대로 따른다. 지금 쿠킹 패키지 v1 로더는 패키지 전체를 읽으므로(에셋 시스템 5.7절 6) 지연 entry가 선행 조건이다(10.1절 FP1).
- Stream entry를 압축하지 않는 이유: FreeType은 glyph 하나를 위해 `glyf`·`CFF ` 표의 작은 범위를 읽는다. 블록 압축(256 KiB)이면 범위마다 블록 전체를 풀어야 한다. LazyLoad entry는 Zstd 블록 압축을 허용한다(전체를 한 번 읽는다).
- 거리장이 켜진 face(`distanceField.enabled`이고 프로젝트 설정이 허용)는 **전처리 geometry**(하위 face별 윤곽 — 6.5절)를 쿠킹 때 만든다. 위치는 원본 byte와 **같은 로딩 방식**을 따른다(Inline이면 일반 entry, LazyLoad·Stream이면 지연 entry, Stream이어도 geometry entry는 압축 가능). UE도 geometry를 `FFontFaceData` 안, 즉 `.ufont`에 함께 둔다(`CFC` 60–80행). CJK 글꼴의 geometry는 크므로 패키지 로드 때 함께 올리지 않는다.
- 서버 타깃 쿠킹은 원본 byte·전처리 geometry를 담지 않는다(5.4절).

### 4.5 실행 중 원본 데이터(`TextLayout` `FontFaceData`)

- `FontFaceData`는 TextLayout의 스레드 안전 공유 객체다(UE `FFontFaceData`와 같은 역할). 내용: 원본 byte(Inline·LazyLoad) 또는 범위 읽기 source(Stream), 전처리 geometry, 상태(NotLoaded·Loading·Ready·Failed).
- `Font` 모듈의 face 공급자가 FontFace 객체마다 하나를 만든다. 객체가 GC로 사라져도 글꼴 cache가 쥔 공유 참조로 byte·source가 산다(UE와 같다). Stream source는 패키지 파일 범위 읽기 handle을 쥐며 마지막 참조가 놓일 때 닫는다.
- **LazyLoad 읽기 시점.** UE 기본(`Slate.Font.AsyncLazyLoad` = false)은 처음 쓸 때 동기로 읽는다. Rocks도 기본은 동기다(측정이 처음부터 맞다). `ui.font.async_lazy_load`(CVar, 기본 false)를 켜면 비동기로 읽고, 그동안 이 face는 "준비 안 됨"으로 측정되며 준비되면 TextLayout이 `FontFaceReady` 사건을 내고 UI가 해당 텍스트의 Layout을 무효화한다. 편집기·Tryout에서는 UE처럼 비동기를 쓰지 않는다.
- 실패(파일 IO·손상 글꼴): face 상태 Failed, 오류 코드 보존(로그 영어), 조합 탐색은 다음 단계로 넘어간다(6.3절). 가짜 metric을 만들지 않는다.

### 4.6 에셋 → TextLayout 연결

- `Font::ResolveCompositeFont()` → `SharedRef<const ResolvedCompositeFont>`: `CompositeFontDesc`의 face 참조를 `FontFaceData` handle로 바꾼 TextLayout 실행용 조합 글꼴. 결과는 Font 객체에 cache하고 편집·재임포트 때 버린다. `runtimeSource` = DefaultStyle이면 엔진 기본 조합 글꼴을 준다.
- UI 글꼴 지정(UiToolkit `FontInfo`)은 `ResolvedCompositeFont`를 가리킨다. 저장되는 쪽(Widget 스타일의 글꼴 필드 — Font 참조·typeface·크기·material 참조)은 UiToolkit 소유다. 이 문서는 TextLayout `FontInfo`의 비 material 필드를 6.1절에서 정한다.
- 서버 타깃은 backend가 없어 face를 열지 못한다(3장). `Resolve`는 성공하되 그 조합 글꼴의 측정·래스터 호출이 `Unsupported`다.
