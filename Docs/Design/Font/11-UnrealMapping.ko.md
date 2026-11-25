## 11. 대응표와 UE와 다르게 정한 점

### 11.1 대응표

| Rocks | UE 5.8 | 위치 |
|---|---|---|
| `FontFace` | `UFontFace` | Font |
| `Font` | `UFont` | Font |
| `CompositeFontDesc`·`TypefaceDesc`·`TypefaceEntryDesc`·`FallbackFontDesc`·`SubFontDesc` | `FCompositeFont`·`FTypeface`·`FTypefaceEntry`·`FCompositeFallbackFont`·`FCompositeSubFont`(+`FFontData`) | Font |
| `ResolvedCompositeFont` | `FCompositeFont`의 실행 탐색 쪽(`FCompositeFontCache`) | TextLayout |
| `FontFaceData` | `FFontFaceData` | TextLayout |
| `FontInfo` · `FontOutline` | `FSlateFontInfo` · `FFontOutlineSettings` | TextLayout |
| `TextBackend`(표) · `TextLayoutBackend`(모듈) | `WITH_FREETYPE`·`WITH_HARFBUZZ` 분기의 SlateCore Private 구현 | TextLayout · ClientOnly 모듈 |
| `UnicodeBlockRanges` | `FUnicodeBlockRange` | TextLayout |
| `FontDistanceFieldSettings` | `UFontFace` 거리장 필드 + `FFontFacePlatformRasterizationOverrides` | Font |
| `PrebakedFontData` · `FontCharacter` | `UFont` Offline 필드 · `FFontCharacter` | Font |
| `PrebakedFontImportOptions` | `FFontImportOptionsData` | Font(EditorOnly) |
| `FontFaceImporter` · `FontCreateMode` | `UFontFileImportFactory` · `EBatchCreateFontAsset` | FontTools |
| `PrebakedFontBuilder` | `UTrueTypeFontFactory` | FontTools |
| `FontMeasure` · `ShapedGlyphSequence` · `TextShaper` · `FontCache` | `FSlateFontMeasure` · `FShapedGlyphSequence` · `FSlateTextShaper` · `FSlateFontCache` | TextLayout |
| 전처리 geometry | `FPreprocessedFontGeometry` | TextLayout |
| `ui.font.async_lazy_load` | `Slate.Font.AsyncLazyLoad` | CVar |
| `ui.font.antialiasing` | `Slate.EnableFontAntiAliasing` | CVar |
| `ui.text.default_shaping_method` | `Slate.DefaultTextShapingMethod` | CVar |
| `ui.font.max_atlas_pages_before_flush` · `ui.font.grow_atlas_frame_window` | `Slate.MaxFontAtlasPagesBeforeFlush` · `Slate.GrowFontAtlasFrameWindow` | CVar |
| `ui.font.max_non_atlas_textures_before_flush` · `ui.font.grow_non_atlas_frame_window` | `Slate.MaxFontNonAtlasTexturesBeforeFlush` · `Slate.GrowFontNonAtlasFrameWindow` | CVar |
| `ui.font.unload_face_data_on_flush` | `Slate.UnloadFreeTypeDataOnFlush` | CVar |
| `ui.font.flush_cache` · `ui.font.dump_cache_stats` | `Slate.FlushFontCache` · `Slate.DumpFontCacheStats` | 콘솔 명령 |
| `ui.font.distance_field_enable` · `ui.font.distance_field_mode` · `ui.font.distance_field_level` | `UI.SlateSDFText.Enable` · `UI.SlateSDFText.RasterizationMode` · `UI.SlateSDFText.ResolutionLevel` | CVar(기기 프로필) |
| `ui.font.distance_field_rasterization` | `UUserInterfaceSettings::bEnableDistanceFieldFontRasterization` | 프로젝트 설정 |
| `ui.font.alpha_atlas_size` · `ui.font.color_atlas_size` · `ui.font.distance_field_atlas_size` | `[SlateRenderer] GrayscaleFontAtlasSize`·`ColorFontAtlasSize`·`SdfFontAtlasSize` | Config |
| 엔진 글꼴 설정(여섯 + 배열) | `UEngine::TinyFont`…`MonospaceFont`·`AdditionalFonts` | Config 엔진 설정 |
| 머티리얼 `FontSample`·`FontSampleParameter`·`FontSignedDistance` | `UMaterialExpressionFontSample`·`…FontSampleParameter`·`…FontSignedDistance` | Material |

대응하지 않는 UE CVar: `Slate.EnableLegacyFontHinting`(레거시 hinting은 들이지 않는다), `Slate.OutlineFontRenderMethod`·`Slate.Font.DetectAsyncLoadingFontFaces`(FT2 구현 때 UE 동작을 확인해 정한다), `UI.Slate.Font.SimulateCookedFontFilesInStandalone`(Rocks는 편집기 실행도 같은 로딩 경로를 시험으로 덮는다).

### 11.2 UE와 다르게 정한 점

| 항목 | UE | Rocks | 결정 |
|---|---|---|---|
| 조합 글꼴 표현 | 한 struct | 저장·실행 둘 | FD2 |
| 서버 분리 | 같은 모듈, `WITH_FREETYPE` 분기 | ClientOnly backend 모듈 + 표 등록 | FD13 |
| cache 방식 이름 | Offline·Runtime | Prebaked·OnDemand | FD4 |
| 원본 경로 필드 | `SourceFilename` | 없음 | FD5 |
| 파일 경로 face | 있음 | 없음(내장 UI 글꼴만 예외) | FD6 |
| 미리 굽기 문자 키 | `uint16` | code point | FD7 |
| 지연 원본 위치 | `.ufont` 별도 파일 | 지연 bulk entry(AssetSystem 결정 따름) | FD8 |
| 미리 굽기 입력 | 시스템 글꼴 이름·GDI | FontFace·글꼴 파일·FreeType | FD11 |
| 미리 굽기 페이지 | Font 하위 Texture2D | 별도 Texture2D 패키지 | FD18 |
| 미리 굽기 페이지 채널 | A 채널 마스크 | R 채널(`DistanceFieldFont` 규칙) | 5.2절 |
| 미리 굽기 문자 구간 | 문자열, 파일 와일드카드 | `CodePointRange` 배열, 파일 하나(결과 목록 저장) | 5.2절 |
| sub-font culture | `;` 문자열 | `Array<Name>` | 4.3.1절 |
| 하위 face마다 entry | 없음(entry 하나) | 선택 `entryPerSubFace` | 5.1절 |
| 거리장 전처리 시점 | 편집기 | 쿠킹 처리기 | 4.4절 |
| 서버 쿠킹 | `.ufont` 포함 | 글리프 데이터 빈 참조 | FD13 |
| KerningOnly kerning | `kern` 표만 | GPOS도 | FD19 |
| Alpha atlas 형식 | `PF_A8` | `R8` | 7.1절 |
| OnDemand 글꼴 `TextRenderComponent` | 조용히 그리지 않음 | 그리지 않음 + 저장 검증 Warning | 7.3절 |
| 라이브러리 버전 | HarfBuzz 2.4.0·ICU 64.1 | 최신 안정판 | FD14 |
| WOFF·WOFF2 | 받지 않음 | FT8에서 받음(Rocks 추가) | FD10 |
| 임포트 묶음 선택 | 대화상자 Yes to All | 임포트 옵션 일괄 적용 | 5.1절 |
