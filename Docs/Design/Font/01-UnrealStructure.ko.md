## 1. UE 5.8의 글꼴 구조

### 1.1 층

UE는 글꼴을 네 층으로 나눈다.

1. **원본 데이터 층(`SlateCore`, 객체 아님).** `FFontFaceData`(`CF` 67–131행)는 원본 글꼴 파일 byte(`TArray<uint8> Data`)와 하위 face별 전처리 geometry(`PreprocessedFontGeometries`)를 담는다. 스레드 안전 공유 참조(`TSharedRef<…, ThreadSafe>`)라 에셋 객체 수명과 무관하게 글꼴 cache가 붙잡는다(`CF` 64–66행 주석).
2. **조합 기술 층(`SlateCore`, USTRUCT).** `FFontData`(face 한 개: face 에셋 참조 또는 파일 이름, hinting, loading policy, sub-face index — `CF` 144–307행), `FTypefaceEntry`(이름 + `FFontData`), `FTypeface`(entry 목록), `FCompositeFallbackFont`(typeface + `ScalingFactor`), `FCompositeSubFont`(fallback + `CharacterRanges`·`Cultures`), `FCompositeFont`(`DefaultTypeface`·`FallbackTypeface`·`SubTypefaces`·`bEnableAscentDescentOverride`, `CF` 427–490행). 글꼴 지정은 `FSlateFontInfo`(`SFI`): `FontObject`(UFont)·`FontMaterial`·`OutlineSettings`(크기·mitered·채움 alpha 분리·그림자에 외곽선·외곽선 material·색)·`TypefaceFontName`·`Size`·`LetterSpacing`·`SkewAmount`·`FontFallback`(`FF_NoFallback`·`FF_LastResortFallback`·`FF_Max`)·`bForceMonospaced`·`MonospacedWidth`·`bMaterialIsStencil`.
3. **에셋 층(`Engine`, UObject).**
   - `UFontFace`(`FF`): 원본 파일 하나. `SourceFilename`, `Hinting`(`EFontHinting` Default·Auto·AutoLight·Monochrome·None), `LoadingPolicy`(`EFontLoadingPolicy` LazyLoad·Stream·Inline), `LayoutMethod`(Metrics·BoundingBox), ascent·descent 재정의(`bIsAscendOverridden`·값, −100–100), `StrikeBrushHeightPercentage`(기본 60), 거리장 설정(`bEnableDistanceFieldRendering`, 단일 채널 ppem 32·48·64, 다중 채널 ppem 32·40·56 — 8–256, 플랫폼 래스터 모드 재정의 `FFontFacePlatformRasterizationOverrides`), 편집 전용 `SubFaces`(TTC·OTC의 하위 face 이름 cache).
   - `UFont`(`FN`): `FontCacheType`(`EFontCacheType` **Offline**: 임포트 때 구운 글리프 텍스처 + 문자 표 / **Runtime**: 조합 글꼴, 실행 중 필요할 때 글리프 cache), `RuntimeFontSource`(Asset·CoreStyleDefault), Offline 데이터(`Characters`(`FFontCharacter` UV·크기·텍스처 번호·세로 오프셋), `Textures`(UTexture2D), `IsRemapped`·`CharRemap`, `EmScale`·`Ascent`·`Descent`·`Leading`·`Kerning`, `ImportOptions`, `ScalingFactor`), Runtime 데이터(`CompositeFont`, `LegacyFontSize` 기본 9, `LegacyFontName`).
4. **처리 층(`SlateCore` Private + `SlateRHIRenderer`).** FreeType face(`SFP` FontCacheFreeType — 메모리 열기·스트림 열기, kerning·advance cache), HarfBuzz shaping(`SFP` FontCacheHarfBuzz, `ETextShapingMethod` Auto·KerningOnly·FullShaping — `FC` 51–71행), 래스터(`SFP` SlateFontRenderer — `FT_Set_Transform` 기울임, `FT_Stroker` 외곽선, `FT_LOAD_COLOR` 컬러 글리프), 거리장 생성(`SFP` SlateSdfGenerator — msdfgen, 전처리 geometry를 Task에서 읽음), 글리프 atlas(`ESlateFontAtlasContentType` Alpha·Color·Msdf, `EFontCacheAtlasDataType` Regular·Outline).

### 1.2 FontFace의 저장·쿠킹·로딩

- 에디터 저장: 원본 byte를 늘 inline으로 패키지에 담는다(`FFC` 127행 `bSaveInlineData = Inline || !IsCooking`).
- 쿠킹: `Inline`이 아니면 원본 byte를 패키지 옆 `<이름>.ufont` 파일로 따로 쓴다(`FFC` 254–275행 `CookAdditionalFilesOverride`). 쿠킹된 객체는 `SourceFilename`을 그 파일 경로로 바꾼다(`FFC` 87–89행).
- 로딩: `LazyLoad`는 처음 쓸 때 `.ufont` 전체를 메모리에 읽는다(비동기 여부 `Slate.Font.AsyncLazyLoad`). `Stream`은 FreeType `FT_Stream`으로 파일을 열어 둔 채 글리프마다 읽는다(메모리 최소, 렌더 중 파일 IO). `Inline`은 패키지와 함께 올라온다(멈춤 없음).
- 편집기 단독 실행에서 쿠킹된 동작을 흉내 내는 `UI.Slate.Font.SimulateCookedFontFilesInStandalone`(`FFC` 32–35행).
- 거리장 래스터를 켠 face는 편집기에서 전처리 geometry를 미리 만든다(`CacheSubFaces` → `EnsurePreprocessedFontGeometry`, `FFC` 284–295행). 결과는 `FFontFaceData::Serialize`가 byte 뒤에 함께 저장한다(`CFC` 60–80행).
- 래스터 모드는 기기 프로필 CVar `UI.SlateSDFText.RasterizationMode`(Bitmap·Msdf·Sdf·SdfApproximation)·`UI.SlateSDFText.ResolutionLevel`(1–3)과 프로젝트 설정(`bEnableDistanceFieldFontRasterization`)이 정한다(`FFC` 37–56행).

### 1.3 임포트

| 경로 | 입력 | 결과 | 근거 |
|---|---|---|---|
| 글꼴 파일 임포트 | `ttf`·`ttc`(TrueType), `otf`·`otc`(OpenType) | `UFontFace`(원본 byte·`SourceFilename`·`CacheSubFaces`). 묶음 선택 `EBatchCreateFontAsset`(Yes·No·CreateIfNoFontExists)으로 같은 이름 `UFont`(Runtime, 기본 typeface = 이 face)도 만든다 | `EF` 5140–5143행, `FFI` 16–29행 |
| 새 글꼴 | — | 빈 `UFont`(편집기에서 조합 구성) | `UFontFactory` |
| 미리 구운 글꼴 | 설치된 시스템 글꼴 이름(`FontName`, 기본 Arial) + `FFontImportOptionsData` | `UFont`(Offline) + 페이지 텍스처 | `TTF`, `TTI` — Windows는 GDI, 그 밖은 FreeType |

`FFontImportOptionsData`(`FIO`, 기본값): `Height` 16, 안티앨리어싱 켬, 굵게·기울임·밑줄, `bAlphaOnly`, 문자 집합(Default·Ansi·Symbol), 문자 지정(`Chars`·`UnicodeRange`·`CharsFilePath`·`CharsFileWildcard`·`bCreatePrintableOnly`·`bIncludeASCIIRange` 켬), `ForegroundColor`, `bEnableDropShadow`, 페이지 256×256, 여백 `XPadding`·`YPadding` 1, 상자 확장 4방향, `Kerning`, 거리장 alpha(`bUseDistanceFieldAlpha`, 배율 16, 탐색 반경 배율 1.0).

재임포트는 FontFace는 `SourceFilename`으로(`FFI` `FReimportHandler`), 미리 구운 글꼴은 임포트 옵션으로 한다(`TTF` 19행 `UTrueTypeFontFactory : FReimportHandler`, `TTI` 223–255행). 거리장 alpha로 구울 때 UE는 안티앨리어싱·그림자를 끄고, 배율을 2 이상의 2의 거듭제곱으로, 탐색 반경 배율을 0–8로, 여백을 최소 4로, 페이지 크기를 4의 배수로 강제한다(`TTI` 183–198행). 탐색 반경은 (height/2) × 배율 × 반경 배율이다(948행). 페이지는 `alphaOnly`여도 `TC_DistanceFieldFont`·sRGB 끔이고 마스크는 A 채널이다(963–977행).

### 1.4 사용처

| 사용처 | Offline | Runtime | 근거 |
|---|---|---|---|
| Slate·UMG 텍스트 | 쓰지 않음 | 씀 | `SFI` `FontObject` |
| Canvas(`DrawText`·`FCanvasTextItem`, 디버그·HUD·콘솔) | 씀(페이지 텍스처) | 씀(Slate 글꼴 cache) | `CI` 945–1011행, `Canvas.cpp` 1811행 |
| `UTextRenderComponent`(월드 공간 3D 글자) | **Offline만**(페이지별 동적 머티리얼 인스턴스 cache — `TRC` 314–376행) | 경고 없이 그리지 않음 | `TRC` 324·432·704·729–734행 |
| 머티리얼 `FontSample`·`FontSampleParameter`(Font + 페이지 번호) | 씀 | — | `Materials/MaterialExpressionFontSample*.h` |
| 머티리얼 `FontSignedDistance`(Slate SDF 글자 머티리얼에 거리장 데이터 공급) | — | 씀 | `MaterialTemplate.ush` 2022행 |
| 엔진 글꼴 설정(`TinyFont`·`SmallFont`·`MediumFont`·`LargeFont`·`SubtitleFont`·`MonospaceFont`·`AdditionalFonts`) | 둘 다 | 둘 다 | `Engine/Classes/Engine/Engine.h` 734–792행 |

### 1.5 기본 글꼴과 마지막 fallback

엔진 기본 조합 글꼴은 Roboto 계열·DroidSansMono·DroidSansFallback·Noto(UiToolkit 4.2절). 쿠킹 안 된 실행에서만 `Engine/Content/SlateDebug/Fonts/LastResort.ttf`를 마지막 fallback으로 쓴다(`LegacySlateFontInfoCache.cpp` 50–51행).

### 1.6 GPU atlas

크기는 회색조 1024(편집기 2048), 컬러 512, 거리장 512(편집기 1024), 설정 범위 128–2048(`SRM` 24–43행). Alpha atlas 형식은 `PF_A8`이다(`SlateRHIFontTexture.cpp` 101–106행). atlas보다 큰 글리프는 atlas 밖 개별 텍스처로 간다(`FontCache.cpp` 1100–1122행, `Slate.MaxFontNonAtlasTexturesBeforeFlush`·`Slate.GrowFontNonAtlasFrameWindow`). flush 관련 네 값의 기본은 모두 1이다(`TextureAtlas.h` 281–287행), `Slate.UnloadFreeTypeDataOnFlush` 기본 1(`FontCache.cpp` 50행). 거리장 글리프는 Msdf·Sdf면 자리표시를 먼저 내고 비동기 생성 뒤 바꾸며, SdfApproximation은 자리표시 방식만 쓴다(`FontCache.cpp` 1340·1463–1467행). 거리장 cache 키에는 바깥·안쪽 spread가 들어가 외곽선 크기와 목표 ppem에 따라 달라진다(1346–1349·1449행). Msdf는 MTSDF 4채널이다(`SlateSdfGenerator.cpp` 448–451행). 페이지가 `Slate.MaxFontAtlasPagesBeforeFlush`를 넘으면 다음 프레임 경계에서 cache를 비운다(`Slate.GrowFontAtlasFrameWindow`). 그 밖의 CVar: `Slate.DefaultTextShapingMethod`, `Slate.EnableFontAntiAliasing`, `Slate.EnableLegacyFontHinting`, `Slate.FlushFontCache`, `Slate.DumpFontCacheStats`, `Slate.OutlineFontRenderMethod`, `Slate.UnloadFreeTypeDataOnFlush`, `Slate.Font.DetectAsyncLoadingFontFaces`, `UI.SlateSDFText.Enable`.

### 1.7 shaping·탐색 세부(`SlateTextShaper.cpp`·`FontCacheCompositeFont.cpp`·`SlateFontRenderer.cpp`)

- 크기 단위는 96 DPI 기준 포인트다(`SlateFontInfo.h` 15행 `RenderDPI = 96` — 픽셀 = size × 96/72). `FSlateFontInfo` 기본값은 `Size` 24, `FontFallback` = `FF_Max`(`SlateFontInfo.cpp` 44–45행).
- hinting → FreeType 플래그(`SlateFontRenderer.cpp` 57–82행): 늘 `FT_LOAD_COLOR`, scalable face면 `FT_LOAD_NO_BITMAP`. 안티앨리어싱 켬일 때 Default → `TARGET_NORMAL`, Auto → `FORCE_AUTOHINT`, AutoLight → `TARGET_LIGHT`, Monochrome → `TARGET_MONO | FORCE_AUTOHINT`, None → `NO_AUTOHINT | NO_HINTING`. 안티앨리어싱 끔이면 hinting과 무관하게 `TARGET_MONO | FORCE_AUTOHINT`. last resort 글꼴은 `FORCE_AUTOHINT`(404행).
- 조합 탐색: 보조 typeface는 culture 우선 구간 표 → 일반 구간 표 순으로 **하나만** 고른다(`FontCacheCompositeFont.cpp` 198–208행). 구간이 겹치면 **작은 구간**이 이기고 크기가 같으면 선언 순서(394–406행). typeface 안에서는 이름 → 속성 best match(`GetBestMatchFontForAttributes`) → 첫 entry(614–645행). 고른 face에 글리프가 없으면 기본 typeface → fallback typeface(fallback은 cmap 검사를 건너뜀, 657–679행).
- 글리프가 어디에도 없으면 U+FFFD로 같은 탐색을 다시 한다(`SlateFontRenderer.h` 41행, `SlateTextShaper.cpp` 599–604행). 공백류는 fallback 없이 공백으로 처리한다(26–32행). face 선택 단위는 grapheme cluster의 첫 code point다(566–575행 — 이모지 ZWJ 열).
- Auto shaping은 LTR이라도 결합 문자·ZWJ·인도계·태국어·티베트어·크메르어 등 복합 문자, 서로게이트 쌍이 있으면 FullShaping이다(154–273행). 자간은 RTL이거나 여러 code point grapheme이 있으면 무시한다(535–583·700–705·713행). 자간이 0이 아니면 `liga`를 끈다(729–735행).
- 컬러 비트맵: 고정 크기 strike를 `FT_Select_Size`로 고르고(`FontCacheFreeType.cpp` 206행) atlas에 넣기 전에 축소한다(`GetBitmapAtlasScale`, 302–322행). non-scalable face는 보조 배율을 무시한다(`SlateTextShaper.cpp` 606–610행).
- 거리장 CVar 기본: 모드 Bitmap, 해상도 단계 2(`FontFace.cpp` 37–47행). ppem은 최종 모드가 Msdf면 msdf 표, 아니면 sdf 표에서 고르며(406–421행) PostLoad·편집 때 계산한다(153·385행).

### 1.8 그 밖의 UE 기능

- 이름 있는 유니코드 블록 표 `FUnicodeBlockRange`(`SlateCore/Public/Fonts/UnicodeBlockRange.h` — 기본 글꼴 구성과 조합 글꼴 편집기의 구간 선택에 쓴다).
- FontEditor의 페이지 내보내기·다시 굽기(`Editor/FontEditor/Private/FontEditor.h` 112–119행).
- 지역화 글꼴 패키지: 쿠킹 파일 이름 계산이 지역화 패키지 경로를 따른다(`FontFace.cpp` 370–377행).
- 머티리얼 `FontSample`의 페이지 번호 범위 오류(`MaterialExpressions.cpp` `UMaterialExpressionFontSample::Compile`).

### 1.9 서버와 서드파티

- `SlateCore.Build.cs` 32–55행: 서버 타깃이면 FreeType·HarfBuzz를 링크하지 않는다(`WITH_FREETYPE=0`·`WITH_HARFBUZZ=0`).
- 실제 선택 버전(`ThirdParty/*.Build.cs`): FreeType 2.14.1(모든 플랫폼, Brotli 미연결 — WOFF2 불가), HarfBuzz 2.4.0(트리에 12.2.0이 있으나 선택되지 않음, 12.2.0 빌드는 `hb-icu.cc`를 따로 컴파일하고 `HAVE_ICU`·`HAVE_ICU_BUILTIN` — `harfbuzz-12.2.0/BuildForUE/CMakeLists.txt` 87·133행), ICU 64.1(트리에 78.1이 있으나 선택되지 않음), msdfgen 단일 파일(tps는 V1.10이나 헤더는 2014–2025 저작권과 1.12 API `SDFTransformation`·`DistanceMapping`을 담는다 — `msdfgen.h` 524–562행, `MSDFGEN_USE_FREETYPE`의 `readFreetypeOutline`을 쓴다 — `SlateSdfGenerator.cpp` 261행), nanosvg. FreeType은 zlib·libPNG에 의존한다(`FreeType2.Build.cs` 67–68행). SlateCore는 ICU·nanosvg도 비서버 타깃에서만 붙인다(`SlateCore.Build.cs` 32–56행). 서버의 ICU는 Core 국제화 쪽이다(`Core.Build.cs` 253–257행).
- 변수 글꼴(variable font)의 축 설정과 COLRv1·OT-SVG 컬러 글꼴은 UE도 쓰지 않는다(`SFP`에 `FT_Set_Var_*`·`FT_Get_Color_Glyph_Paint` 호출 없음).
