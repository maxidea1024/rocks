## 6. TextLayout 글꼴 코어

UiToolkit 3.12절 표의 "글꼴 정보·조합 글꼴·글리프 cache·shaping·SDF 글꼴" 행을 상세화한다. 모두 `rocks::text` 이름공간, CPU만으로 동작·시험한다(GPU 업로드는 7.1절). 라이브러리를 부르는 부분은 `TextLayoutBackend`(ClientOnly)가 backend 표로 구현하고(3장), FreeType·HarfBuzz·ICU·msdfgen 타입은 그 모듈의 Private에만 있다. 아래 규칙은 UE 소스 근거(1.7절 shaping·탐색 세부)를 따른다.

### 6.1 글꼴 지정(`FontInfo`)

UE `FSlateFontInfo`(`SFI`)의 material을 뺀 부분이다. material 두 개(`FontMaterial`·`OutlineMaterial`)와 `bMaterialIsStencil`은 UiToolkit 스타일·UiRender 몫이며 `FontInfo`에는 불투명 handle 자리만 둔다.

| 필드 | 기본값 | UE |
|---|---|---|
| `font: SharedPtr<const ResolvedCompositeFont>` | null(= 엔진 기본) | `FontObject`·`CompositeFont` |
| `typeface: Name` | None(= 6.3절 typeface 안 선택 규칙) | `TypefaceFontName` |
| `size: float` | 24(1–1000). **단위는 96 DPI 기준 포인트**(픽셀 = size × 96/72, UI 배율 1.0일 때) | `Size`, `RenderDPI` = 96 |
| `letterSpacing: int32` | 0(−1000–10000, 1/1000 em) | `LetterSpacing` |
| `skew: float` | 0(−5–5) | `SkewAmount` |
| `fallback: FontFallback` | **All** | `FontFallback` = `FF_Max` |
| `monospaced: bool` · `monospacedWidth: float` | false · 1.0(em) | `bForceMonospaced` · `MonospacedWidth` |
| `outline: FontOutline` | {size 0, miteredCorners false, separateFillAlpha false, applyToDropShadows false, color 검정} | `FFontOutlineSettings` |

같음·hash는 UE처럼 모든 필드로 정의한다(글리프 cache 키의 일부).

### 6.2 face와 metric

- `FontFaceHandle`은 (`FontFaceData`, 하위 face 번호, hinting, layout method, ascent·descent 재정의)로 정해지는 열린 face다. 같은 키는 한 번만 연다(face cache). 열기: Inline·LazyLoad는 메모리 열기, Stream은 `FT_Stream` 콜백이 `FontFaceData`의 범위 읽기 source를 부른다(UE `FontCacheFreeType`와 같다).
- FreeType face는 스레드 안전하지 않다. face마다 잠금 하나를 두며, 이 잠금은 HarfBuzz shaping이 `hb-ft`를 거쳐 부르는 FreeType 호출까지 덮는다. FreeType library 객체는 backend 인스턴스마다 하나다. 거리장 생성 Task는 face를 쓰지 않고 전처리 geometry만 읽는다(6.5절).
- metric(크기·배율별): ascender·descender·줄 높이·밑줄 위치·두께·취소선 위치(`strikeHeightPercent`), `layoutMethod` = BoundingBox면 face 경계 상자로 계산. 26.6 고정소수를 float UI 단위로 바꾼다. 조합 글꼴의 `enableAscentDescentOverride`가 거짓이면 face의 재정의를 무시한다(UE와 같다).
- kerning: `KerningOnly` shaping은 `FT_Get_Kerning`으로, `FullShaping`은 HarfBuzz가 GPOS로 한다. 쌍 cache를 face·크기별로 둔다. `FT_Get_Kerning`은 옛 `kern` 표만 읽으므로 GPOS만 있는 글꼴은 KerningOnly에서 kerning이 없다. UE는 FreeType `TT_CONFIG_OPTION_GPOS_KERNING`을 끈다. Rocks는 이 옵션을 **켠다**(FD19 — GPOS 쌍 kerning을 KerningOnly에서도 읽음, UE보다 나은 쪽이며 FullShaping 결과와 더 가깝다).
- **hinting → FreeType 플래그**(UE `SlateFontRenderer.cpp` 57–82행과 같다):

| 조건 | 플래그 |
|---|---|
| 늘 | `FT_LOAD_COLOR` |
| scalable face | + `FT_LOAD_NO_BITMAP` |
| 안티앨리어싱 켬, Default | `FT_LOAD_TARGET_NORMAL` |
| 안티앨리어싱 켬, Auto | `FT_LOAD_FORCE_AUTOHINT` |
| 안티앨리어싱 켬, AutoLight | `FT_LOAD_TARGET_LIGHT` |
| 안티앨리어싱 켬, Monochrome | `FT_LOAD_TARGET_MONO \| FT_LOAD_FORCE_AUTOHINT` |
| 안티앨리어싱 켬, None | `FT_LOAD_NO_AUTOHINT \| FT_LOAD_NO_HINTING` |
| 안티앨리어싱 끔(`ui.font.antialiasing` = false) | hinting과 무관하게 `FT_LOAD_TARGET_MONO \| FT_LOAD_FORCE_AUTOHINT` |
| last resort 글꼴 | `FT_LOAD_FORCE_AUTOHINT` |

### 6.3 조합 글꼴 탐색

`ResolvedCompositeFont`에서 글자 하나에 쓸 face를 고르는 규칙(UE `FontCacheCompositeFont.cpp`와 같다):

1. **선택 단위**는 grapheme cluster의 첫 code point다(이모지 ZWJ 열이 한 face로 간다). 공백류는 탐색하지 않고 공백으로 처리한다.
2. **typeface 고르기**: culture 우선 구간 표(현재 culture 또는 그 부모가 `cultures`에 있는 보조 typeface들의 구간) → 일반 구간 표(culture 지정이 없는 보조 typeface들의 구간) 순으로 code point를 포함하는 구간을 찾아 **보조 typeface 하나**를 고른다. 구간이 겹치면 **작은 구간**이 이기고, 크기가 같으면 선언 순서다. 없으면 기본 typeface다.
3. **typeface 안 face 고르기**: `FontInfo::typeface` 이름이 맞는 entry → 이름이 없으면 속성(굵기·기울임 — face의 style 속성) best match → 첫 entry.
4. 고른 face의 `cmap`에 글리프가 없으면 기본 typeface → fallback typeface(`scalingFactor` 적용, cmap 검사 없이 받음) 순으로 넘어간다. culture 없는 다른 보조 typeface로는 넘어가지 않는다.
5. `FontInfo::fallback`이 LastResort 이상이면 마지막 fallback 글꼴(7.5절, 쿠킹되지 않은 실행에서만 있다).
6. 그래도 없으면 **U+FFFD로 1–5를 다시** 한다.

- culture는 인자로 받는다(`SetCulture(Name)`). Localization 모듈이 생기면 culture 변경 통지를 받아 탐색 cache를 비운다(UiToolkit 3.12절 지역화 행).
- 구간 표는 `ResolvedCompositeFont`를 만들 때 정렬·병합해 이진 탐색한다(UE `Slate.Font.NormalizeCachedFontRanges` 기본 true와 같은 효과).
- 같은 문자열 안에서 face가 바뀌는 지점이 shaping run 경계가 된다(6.4절).

### 6.4 shaping 경계

- 입력: UTF-8 문자열 범위, BiDi run(UiToolkit `TextBoundary`가 backend의 ICU로 만든다), `FontInfo`, 방식(`TextShapingMethod`, 기본은 CVar `ui.text.default_shaping_method` = Auto — UE `Slate.DefaultTextShapingMethod`).
- **Auto**: RTL run은 FullShaping. LTR run도 결합 문자·ZWJ·복합 문자 script(인도계·태국어·라오어·티베트어·크메르어·버마어 등 UE 목록)·서로게이트 쌍(이모지)이 있으면 FullShaping, 아니면 KerningOnly(UE `SlateTextShaper.cpp` 154–273행 판정을 그대로 옮긴다).
- FullShaping은 HarfBuzz(`hb-ft`로 FreeType face를 공유, script·language는 ICU script 판정과 culture)로, 결과는 글리프 번호·cluster(원문 byte 위치)·advance·offset 목록(`ShapedGlyphSequence`, UE `FShapedGlyphSequence`).
- 고정폭(advance = `monospacedWidth × size × 배율`)·자간(`letterSpacing × size / 1000`)은 shaping 뒤 advance에 적용한다. 자간이 0이 아니면 표준 합자(`liga`)를 끈다. RTL run이거나 여러 code point로 된 grapheme이 있는 run에서는 자간을 무시한다(UE `SlateTextShaper.cpp` 436·444·535–583·700–713·729–735행).
- 측정 API(`FontMeasure`, UE `FSlateFontMeasure`): 문자열 크기, 문자 위치 → 좌표, 좌표 → 문자 위치(grapheme 단위), 줄 높이·baseline.

### 6.5 래스터

| 모드 | 방법 | atlas 종류 |
|---|---|---|
| Bitmap | FreeType 래스터(크기·배율·기울임마다 따로). 기울임은 `FT_Set_Transform` | Alpha |
| 컬러 글리프 | `FT_LOAD_COLOR`(CBDT·sbix 비트맵, COLRv0 층 — FreeType이 합성). 비트맵 face는 가장 가까운 고정 크기 strike를 `FT_Select_Size`로 고르고 atlas에 넣기 전에 목표 크기로 축소한다. non-scalable face는 보조 typeface 배율을 무시한다 | Color(sRGB RGBA) |
| 외곽선 | `FT_Stroker`(mitered·round), 채움과 외곽선을 따로 cache(UE `EFontCacheAtlasDataType` Regular·Outline) | Alpha |
| Msdf | 전처리 geometry → msdfgen **MTSDF**(4채널: 다중 채널 + 실제 거리), Task에서 생성. 먼저 자리표시(빈 글리프, 같은 metric)를 내고 생성이 끝나면 바꾼다 | Msdf(linear RGBA) |
| Sdf | 전처리 geometry → msdfgen 단일 채널, 자리표시 후 교체 | Alpha |
| SdfApproximation | 근사 거리 변환. UE와 같이 자리표시 방식만 쓴다(비동기 교체 없음) | Alpha |

- 거리장 글리프는 크기·기울임과 무관하게 재사용되지만, cache 키에는 **바깥·안쪽 spread**가 들어간다. spread는 외곽선 크기와 목표 ppem에서 정해진다(UE `FontCache.cpp` 1346–1349·1449행). 그래서 "글리프마다 한 번"이 아니라 "(글리프, ppem, spread)마다 한 번"이다.
- 거리장 ppem: face의 `distanceField` low·medium·high 중 `ui.font.distance_field_level`(1–3, 기본 2 — UE `UI.SlateSDFText.ResolutionLevel`)이 고른 값. 최종 모드가 Msdf면 `msdfPpem`, 아니면 `sdfPpem` 표에서 고른다. 모드는 `ui.font.distance_field_mode`(Bitmap·Msdf·Sdf·SdfApproximation, 기본 Bitmap — UE `UI.SlateSDFText.RasterizationMode`)이며 face의 `modeOverride`가 있으면 그 표를 거친다. 계산은 face 로드 뒤와 편집 때, 그리고 CVar 변경 때 한다. 두 CVar은 기기 프로필 값이다([CVar](../CVarSystem.ko.md)·[Config](../ConfigSystem.ko.md) CF4). 거리장 기능 전체 스위치는 `ui.font.distance_field_enable`(UE `UI.SlateSDFText.Enable`).
- **전처리 geometry**(UE `FPreprocessedFontGeometry`): 하위 face별 모든 글리프의 윤곽(contour·edge·정수 좌표·winding)을 담는다. msdfgen 색 배정(edge coloring)은 저장하지 않고 생성 때 한다(UE `SlateSdfGenerator.cpp` 448행과 같다). 쿠킹 때 만들고(4.4절), 없으면(편집기·쿠킹 전) 처음 쓸 때 face 잠금 안에서 만든다. 윤곽 변환은 UE처럼 msdfgen의 FreeType 연결(`ext/import-font.cpp`의 윤곽 읽기)을 쓴다(8.2절). 형식·버전은 TextLayout이 소유하며 버전이 다르면 다시 만든다.
- **결정성**: 쿠킹 산출물(전처리 geometry)은 정수 경로라 플랫폼 사이 bit 일치를 요구한다(5.3절). 실행 중 거리장 생성·래스터는 cache 내용일 뿐 저장되지 않으므로 플랫폼마다 golden을 둘 수 있다(10.3절).

### 6.6 글리프 cache(CPU 부분)

- 키: (`FontFaceHandle`, 글리프 번호, 크기 × 배율(거리장이면 ppem·spread), 기울임, 외곽선 크기·모서리, 모드). 값: atlas 종류·페이지·사각형·bearing·advance.
- atlas 페이지 할당(선반 방식, 1 px 여백 — UE와 같다)과 페이지 수 상한은 TextLayout이 한다. 크기: Alpha 1024(편집기 2048), Color 512, 거리장 512(편집기 1024), 설정 범위 128–2048(UE `SRM` 37–43행 — Config `ui.font.alpha_atlas_size`·`color_atlas_size`·`distance_field_atlas_size`).
- **atlas 밖 글리프**: 한 변이 atlas 크기를 넘는 글리프는 atlas 밖 개별 텍스처로 둔다(UE `FontCache.cpp` 1100–1122행). 개수 상한 `ui.font.max_non_atlas_textures_before_flush`, 성장 창 `ui.font.grow_non_atlas_frame_window`.
- 페이지가 `ui.font.max_atlas_pages_before_flush`를 넘으면 다음 프레임 경계에서 cache 전체를 비운다. `ui.font.grow_atlas_frame_window` 프레임 수 안에 다시 넘으면 비우지 않고 늘린다. 네 값의 기본은 UE와 같이 모두 1이다(`TextureAtlas.h` 281–287행). `ui.font.unload_face_data_on_flush`(기본 true — UE `Slate.UnloadFreeTypeDataOnFlush` = 1)면 비울 때 face도 닫는다.
- 진단 명령: `ui.font.flush_cache`(UE `Slate.FlushFontCache`), `ui.font.dump_cache_stats`(UE `Slate.DumpFontCacheStats`).
- 수치 상한·기본값은 이름 있는 상수로 둔다(전체 규칙).

### 6.7 스레드와 lane

UiToolkit 3.4절 lane 모델을 따른다: 측정·shaping·cache 조회는 UI lane, 거리장 생성은 Task, GPU 업로드는 Render lane 제출. Canvas(게임 lane)가 같은 cache를 쓸 때는 UI lane과 같은 프레임 단계에서 접근하도록 Canvas 설계가 정한다(7.2절). TextLayout은 cache 접근에 잠금 하나를 두어, 정해진 단계 밖 호출도 경쟁 없이 직렬화된다.

### 6.8 FT2 하위 단계와 FT2a API(2026-11-25, 사용자 결정으로 UT2 전에 착수)

| 하위 단계 | 내용 |
|---|---|
| FT2a | 모듈 골격(`TextLayout` Runtime·`TextLayoutBackend` ClientOnly), backend 생성·라이브러리 초기화, face 열기·하위 face 수·face 정보·metric·글리프 번호 |
| FT2b | 조합 글꼴 탐색(6.3절) |
| FT2c | shaping·측정(6.4절) |
| FT2d | 래스터·글리프 cache CPU(6.5–6.6절) |
| FT2e | 전처리 geometry·거리장(6.5절) |

**FT2a API.**
- `TextLayout`(Runtime, Core만 의존):
  - `TextLayout/FontTypes.h`: `FontHinting`·`FontLayoutMethod`(4.1절 값), `FontFaceInfo`(family·style 이름, 글리프 수, unitsPerEm, scalable, 컬러, 고정 크기 수), `FontMetrics`(픽셀 단위 ascender·descender(음수)·줄 높이·밑줄 위치·두께), `ComputeFontPixelSize(size, scale)` — UE `ComputeFontPixelSize`와 같은 식(26.6 고정소수로 `size × 96 / 72`를 반올림해 배율을 곱하고 픽셀로 반올림, `FontCacheFreeType.cpp` 118–135행).
  - `TextLayout/FontFaceData.h`: 불변 원본 byte(`TryCreate(allocator, bytes)`가 복사, 스레드 안전 `SharedPtr`로 공유 — 4.5절).
  - `TextLayout/TextBackend.h`: `TextBackend`(`CountSubFaces`·`OpenFace(FontFaceDesc)`)와 `BackendFace`(`Info`·`Metrics(size, scale)`·`GlyphIndex(codePoint)`). `FontFaceDesc` = (data, 하위 face 번호, hinting, layout method, ascent·descent 재정의).
- `TextLayoutBackend`(ClientOnly): `InitializeTextLibraries(icuData)` — 프로세스에서 한 번 ICU 할당 hook(`u_setMemoryFunctions`)과 데이터(`udata_setCommonData`)를 건다. 첫 ICU·HarfBuzz 사용 전에 불러야 하고, 같은 데이터로 다시 부르면 성공, 다른 데이터면 `InvalidArgument`. 데이터는 프로세스 끝까지 살아야 한다. `CreateTextBackend(options)` — 초기화 뒤 FreeType library(Rocks `FT_Memory`)를 가진 backend를 만든다. HarfBuzz 할당 hook(`RocksHarfBuzz*`)과 ICU hook은 이 모듈이 프로세스 수명 Rocks 할당기로 정의한다(HarfBuzz·ICU의 정적 객체가 프로세스 종료 때 풀리므로). `u_cleanup`은 부르지 않는다(프로세스 종료가 정리한다 — HarfBuzz가 붙잡은 ICU 객체를 끊지 않게).
- metric은 UE와 같다(`FontCacheFreeType.cpp` 257–297행, `FontCacheFreeType.h` 269–293행): scalable face는 글꼴 단위 값(`ascender`·`descender`·`height`, BoundingBox면 `bbox`)에 `y_scale`을 곱하고, 재정의 ascent는 26.6 올림·descent는 내림, 재정의가 있으면 줄 높이 = ascent − descent.
- 오류: 글꼴이 아니거나 손상 `InvalidArgument`(원본 FreeType 오류 문구를 로그), 하위 face 번호 범위 밖 `InvalidArgument`, 초기화 전 `CreateTextBackend` `NotReady`, 할당 실패 `OutOfMemory`.
- ICU·HarfBuzz를 직접 부르는 FT1 연결 시험(`ThirdPartyIcuTests`·`ThirdPartyHarfBuzzTests`)은 FT2a에서 지운다. 시험 실행 파일이 ICU를 직접 의존하면 전역 상태 소비 모듈이 둘(시험 실행 파일·`TextLayoutBackend`)이 되고, Modular 빌드에서는 ICU가 두 벌이 되어 backend가 초기화한 쪽과 시험이 부르는 쪽이 갈라진다(모듈 시험 묶음은 include 소유 검사만 바꾸고 컴파일 환경은 시험 실행 파일 것이다). 같은 검증(태국어 사전 경계, ZWJ grapheme, BiDi run, ICU 기본 유니코드 함수, 합자·아랍어 shaping)은 FT2b·FT2c에서 backend API로 다시 한다. FreeType·libpng·msdfgen 연결 시험은 전역 상태가 없어 그대로 둔다.
- `TextLayoutBackend`는 내리지 않는 모듈이다(`SupportsUnload` = false — backend vtable과 라이브러리 할당 hook이 이 모듈 코드에 있다).

**FT2b API(조합 글꼴 탐색, 6.3절).**
- `TextLayout/CompositeFont.h`: 실행용 조합 기술 `CompositeFontSpec`(기본 typeface·fallback typeface와 배율·보조 typeface 목록·마지막 fallback face — 각 typeface는 이름 있는 `FontFaceDesc` entry 목록, 보조는 `CodePointRange` 구간과 culture 목록)을 받아 `ResolvedCompositeFont::TryCreate(allocator, backend, spec)`가 모든 face를 연다. `SetCulture(culture)`는 culture 우선 구간 표를 다시 만든다(culture와 그 부모 — `ko-KR` → `ko`, `-`·`_` 구분). `Select(codePoint, typefaceName, style, fallback)` → `FontSelection`(face·배율·글리프 번호·실제로 고른 code point).
- 규칙(UE `FontCacheCompositeFont.cpp`와 같다): culture 우선 구간 → 일반 구간에서 보조 typeface 하나(겹치면 작은 구간, 같으면 선언 순서), 없으면 기본 typeface. typeface 안은 이름 → 속성(굵기·기울임 — face style 플래그) 일치 수 최대 → 첫 entry. 고른 face에 글리프가 없으면 기본 → fallback(글리프 검사 없이 받음) → `fallback` ≥ LastResort이면 마지막 fallback face. 그래도 글리프가 0이면 U+FFFD로 다시 고르고, 그것도 없으면 처음 선택(`.notdef`)을 준다.
- cultures가 지정된 보조 typeface는 그 culture(또는 부모)일 때만 쓰고 다른 culture에서는 무시한다(UE `Cultures` 주석과 같다). 공백 처리와 grapheme 단위 선택은 FT2c(shaping)가 한다.
