## 5. 임포트·재임포트·쿠킹

임포트 체계(ImportRecord·재임포트·임포트 상태)는 [에셋 시스템](../AssetSystem/README.ko.md) A2f를, 진행 보고는 [진행 보고](../ProgressReporting/README.ko.md)를 그대로 쓴다. 글꼴 몫은 아래뿐이다. 검사·래스터는 호스트가 등록한 TextLayout backend로 한다(3장 — 도구 프로그램이 `TextLayoutBackend`를 링크한다).

### 5.1 글꼴 파일 임포터(`FontFaceImporter`)

- **입력 확장자**: `ttf`·`ttc`(TrueType), `otf`·`otc`(OpenType) — UE와 같다(`EF` 5140–5143행). 확장자가 아니라 내용으로 판정한다: sfnt 버전 태그 `0x00010000`·`'true'`·`'OTTO'`, collection 태그 `'ttcf'`. 확장자와 내용이 어긋나면 내용을 따르고 Warning을 남긴다. WOFF·WOFF2는 1단계 범위 밖이다(FT8, FD10).
- **검사**(backend로 열어 본다): 열기 실패·하위 face 0개·`maxp`/`head`/`hhea`/`cmap` 표 누락·글꼴 단위(unitsPerEm) 0은 임포트 오류(`InvalidFormat`, 원본 FreeType 오류 코드 보존). 비트맵 전용 글꼴(윤곽 없음)은 받되, 거리장을 켤 수 없다는 진단을 단다.
- **결과**: FontFace 하나. `sourceData` = 파일 byte, ImportRecord = 원본 경로·SHA-256·임포터 버전. 설정(hinting 등)은 기본값이고 재임포트 때 유지한다.
- **같은 이름 Font 생성**: 임포트 옵션 `createFont`(`FontCreateMode` = Yes·No·IfMissing, 기본 Yes — UE `EBatchCreateFontAsset`의 Yes·No·CreateIfNoFontExists). 만들면 UE와 같이 같은 폴더에 `<FontFace 이름>_Font`, 이미 있으면 기존 고유 이름 규칙으로 번호를 붙인다(UE `CreateUniqueAssetName` — `EF` 5156·5226–5229행). `cacheType` = OnDemand, 기본 typeface entry는 UE와 같이 하나(이름 "Default", 하위 face 0 — 5242–5244행)다. TTC·OTC면 하위 face마다 entry를 더하는 선택 `entryPerSubFace`(기본 false, 이름은 하위 face의 style 이름)를 둔다 — Rocks 추가 기능(11.2절). UE의 대화상자 묶음 선택(Yes to All 등)은 Rocks 임포트 옵션의 일괄 적용으로 대신한다.
- **재임포트**: 같은 경로·새 byte. SHA-256이 같으면 아무것도 바꾸지 않는다. 바뀌면 `sourceData`를 바꾸고, 하위 face 수가 줄어 기존 Font entry의 `subFaceIndex`가 범위를 벗어나면 그 Font들에 저장 검증 진단을 단다(자동 수정하지 않는다).

### 5.2 미리 구운 글꼴 생성기(`PrebakedFontBuilder`)

UE `UTrueTypeFontFactory`(`TTF`·`TTI`) 대응. UE는 **설치된 시스템 글꼴 이름**으로 Windows GDI(그 밖은 FreeType)를 써서 굽는다. Rocks는 **FontFace 에셋 또는 글꼴 파일을 입력**으로 받고 모든 플랫폼에서 TextLayout backend(FreeType)로 굽는다(FD11 — OS 설치 글꼴·GDI에 기대면 결과가 기기마다 다르다, FR6·FR7).

**생성 시점은 임포트다**(UE와 같다). 결과(문자 표·metric·페이지 TextureSource)는 Font와 페이지 패키지에 저장되고, 쿠킹은 그것을 텍스처처럼 처리할 뿐 다시 굽지 않는다. 옵션이나 입력 face가 바뀌면 사용자가 재임포트한다(UE와 같다 — `TTI` 223–255행). 입력 face·문자 파일은 쿠킹 Build 의존이 아니라 **임포트 입력**이다: FontFace 입력이면 ImportRecord에 그 face의 AssetId와 `sourceData` SHA-256을, 문자 파일이면 경로와 SHA-256을 남겨 재임포트 판정에 쓴다(에셋 시스템 16장 — Build 의존은 에셋 사이에서만 선언한다).

`PrebakedFontImportOptions`(`ROCKS_STRUCT`, UE `FFontImportOptionsData` 대조):

| Rocks | 기본값 | UE |
|---|---|---|
| `face: SoftObjectRef<FontFace>` 또는 `sourceFile` · `subFaceIndex` | — · 0 | `FontName`(시스템 글꼴 이름) |
| `height` | 16.0(px) | `Height` |
| `antialiasing` · `bold` · `italic` · `underline` | true · false · false · false | `bEnableAntialiasing`·`bEnableBold`·`bEnableItalic`·`bEnableUnderline` |
| `alphaOnly` | false | `bAlphaOnly` |
| `characterSet` | Default(Default·Ansi·Symbol) | `CharacterSet` |
| `characters` · `unicodeRanges` · `charactersFile` | "" · [] · "" | `Chars` · `UnicodeRange`(문자열) · `CharsFilePath`+`CharsFileWildcard` |
| `printableOnly` · `includeAsciiRange` | false · true | `bCreatePrintableOnly` · `bIncludeASCIIRange` |
| `foregroundColor` · `dropShadow` | 흰색 · false | `ForegroundColor` · `bEnableDropShadow` |
| `pageWidth` · `pageMaxHeight` | 256 · 256 | `TexturePageWidth` · `TexturePageMaxHeight` |
| `padding` | {x 1, y 1} | `XPadding`·`YPadding` |
| `extendBox` | {top 0, bottom 0, left 0, right 0} | `ExtendBox*` |
| `kerning` | 0 | `Kerning` |
| `distanceFieldAlpha` · `distanceFieldScale` · `distanceFieldScanRadiusScale` | false · 16 · 1.0 | `bUseDistanceFieldAlpha` · `DistanceFieldScaleFactor` · `DistanceFieldScanRadiusScale` |

- `bold`·`italic`: 입력 face가 그 스타일이 아니면 FreeType 합성(`FT_GlyphSlot_Embolden`·`FT_GlyphSlot_Oblique` — `ftsynth.c`)으로 만든다. UE GDI 경로의 합성 굵게·기울임과 같은 의미다.
- `unicodeRanges`는 `CodePointRange` 배열이다(UE는 "0000-007F,AC00-D7A3" 같은 문자열 — 파싱 오류가 늦게 드러난다). `charactersFile`은 파일 하나이며 와일드카드는 받지 않는다. 임포트 때 읽어 **결과 문자 목록을 옵션에 함께 저장**한다(파일이 사라져도 재생성할 수 있게).
- 배치: 문자를 code point 순서로, 페이지 폭 안에서 왼쪽→오른쪽·위→아래 줄 채움(UE `TTI`와 같은 선반 방식). 결정적이다.
- 거리장 alpha: UE와 같은 강제값을 쓴다 — 안티앨리어싱·그림자 끔, `distanceFieldScale`은 2 이상의 2의 거듭제곱으로 올림, `distanceFieldScanRadiusScale`은 0–8로 자름, `padding`은 최소 4, 페이지 크기는 4의 배수로 올림(`TTI` 183–198행). `height × distanceFieldScale`로 고해상도 래스터 후 탐색 반경 (height/2) × 배율 × 반경 배율(948행)로 거리장을 만들어 축소한다.
- **페이지 형식과 채널**: UE는 `alphaOnly`여도 페이지를 `TC_DistanceFieldFont`·sRGB 끔으로 만들고 마스크를 A 채널에 둔다(`TTI` 963–977행). Rocks 텍스처 압축 설정 `DistanceFieldFont`는 R 채널을 읽는다([에셋 시스템](../AssetSystem/12-Textures.ko.md) 12.4.3절 132행). 그래서 Rocks 페이지 TextureSource는 **마스크(또는 거리장)를 R 채널**에 두고, `alphaOnly`·거리장이면 `DistanceFieldFont` 단일 채널, 아니면 RGBA8(sRGB 끔, 색 = `foregroundColor`, 마스크는 A에도 복사)이다. 머티리얼 `FontSample`의 A 출력은 페이지 형식에 맞춰 R 또는 A를 준다(7.4절).
- 결과: Font(`cacheType` = Prebaked) + Texture2D 페이지 패키지(4.3.2절). 페이지 내보내기·다시 굽기 명령(UE FontEditor 112–119행)은 편집기 몫이다(5.5절).
- **결정성**: 정수·고정소수 경로(FreeType 래스터, 배치)는 플랫폼 사이 bit 일치를 요구한다. 거리장 alpha 축소는 float 경로라 플랫폼 사이 bit 일치를 보장하지 않으므로, 임포트 결과를 저장해 쓰는 방식(쿠킹 때 다시 계산하지 않음)으로 쿠킹 결정성을 지킨다.

### 5.3 쿠킹 처리기(`FontFaceCookProcessor`·`FontCookProcessor`)

- FontFace: 로딩 방식별 bulk 배치(4.4절), 거리장 전처리 geometry 생성(6.5절), EditorOnly 필드 제외. 너겟 키에 처리기 버전·TextLayout 글꼴 코어 버전·FreeType 버전·msdfgen 버전(geometry 형식이 msdfgen 입력이라)·프로젝트 설정 `ui.font.distance_field_rasterization` 값을 넣는다.
- 전처리 geometry는 FreeType 윤곽(26.6 고정소수)에서 정수 좌표로 만든다(UE `FPreprocessedFontGeometry`도 int16 좌표). 그래서 플랫폼 사이 bit 일치를 요구하고 너겟 키에 호스트를 넣지 않는다(오디오 선례 — 너겟 키는 기기마다 같아야 한다).
- Font: `compositeFont` 검증(4.3.1절)을 다시 한다. Prebaked 페이지는 텍스처 처리기가 쿠킹한다.
- 거리장 사용 여부(프로젝트 설정 `ui.font.distance_field_rasterization` — UE `UUserInterfaceSettings::bEnableDistanceFieldFontRasterization` 대응, 기본 false)가 거짓이면 전처리 geometry를 만들지 않는다.

### 5.4 서버 타깃 쿠킹

서버 타깃은 글꼴을 그리지 않는다(FR8). 쿠킹은 FontFace의 원본 byte·전처리 geometry entry를 쓰지 않고, 그 `BulkDataRef`를 **빈 참조**(id 0·크기 0·SHA 0)로 바꿔 쓴다. 지금 패키지 형식은 EditorOnly가 아닌 `BulkDataRef`에 entry가 없으면 로드가 `CorruptPackage`라([에셋 시스템](../AssetSystem/05-PackageFormat.ko.md) 5.2·5.7절), 쿠커에 "타깃별 payload 제외" 절차와 빈 참조 규칙을 더해야 한다(선행 FP6 — 오디오 AD24 "서버 쿠킹은 오디오 payload 제외"와 같은 절차를 함께 쓴다). Prebaked 페이지 Texture2D의 서버 쿠킹은 텍스처의 서버 규칙(픽셀 제외)을 따른다. 서버에서 이 byte를 요청하면 `Unsupported`다. UE는 서버 쿠킹에서도 `.ufont`를 쓴다 — Rocks는 계획 9.6절 908행에 따라 뺀다(11.2절).

### 5.5 편집기(UiToolkit UT4 이후)

UE `FontEditor` 모듈 대응(FontFace 편집기, 조합 글꼴 편집기 `SCompositeFontEditor`, 미리보기 viewport, FontFace 세부 정보, 미리 구운 글꼴 페이지 내보내기·다시 굽기 — `Editor/FontEditor/Private`)은 [UI 툴킷](../UiToolkit.ko.md) UT4의 자산 편집기 host 위에 만든다. 이 문서가 정하는 것은 편집기가 쓸 데이터 API(4장)와 미리보기용 측정·래스터 API(6장)다.
