## 8. 서드파티 라이브러리(FT1)

엔진 코드를 바꾸지 않는 단계라 고정 순서와 무관하게 지금 할 수 있다(README). 도입 방식은 기존 ThirdParty 모듈(예: `Engine/Source/ThirdParty/Opus`)과 같다: 폴더마다 `ThirdParty.json`(출처 URL·SHA-256·검증 방법·SPDX·고지 의무·번역 단위·include·define·adapter 헤더·파일별 SHA-256·pragma 지시문·notes) + `<Lib>.Build.cs`(`ThirdPartyModuleRules`). 원본은 `upstreamDirectory`에 byte 그대로 보존한다([코딩 스타일](../../CodingStyle.md) 82·89행). 라이브러리 폴더에는 manifest·규칙·원본 디렉터리·glue 번역 단위·patch만 둘 수 있으므로(정책 `third-party`), 설정 헤더를 바꿔야 하면 원본 파일을 **추적 가능한 수정**(`modifications` + `Rocks.patch`, 파일마다 이유 — MikkTSpace·Flac 선례)으로 고친다. `files`·`pragmaDirectives`는 갱신 도구(`Rocks.ToolTests third-party --root . --write`)로 계산한다.

### 8.1 목록과 버전

버전은 **도입 시점의 최신 안정판**으로 하고, 하한은 UE 5.8 트리에 있는 최신판이다(FD14). UE가 실제로 고른 HarfBuzz 2.4.0·ICU 64.1은 UE 자체의 하위호환 사정이라 따르지 않는다(1.9절). 2026-11-24 확인한 판:

| 모듈 | 라이브러리·판(2026-11-24 최신) | 하한(UE 5.8 트리) | 검증 | 들이는 범위 | 라이선스(도입 때 원문 대조) |
|---|---|---|---|---|---|
| `FreeType` | FreeType 2.15.0(2026-10-12) | 2.14.1 | savannah 배포 `.tar.xz` + GPG 서명(Werner Lemberg, 지문 `E306 7470 7856 409F F194 8010 BE6C 3AAC 63AD 8E3F`) | `include/`, `src/` 중 8.2절 목록 | FTL(GPLv2와 이중 중 FTL 선택, 문서에 credit 문구) |
| `LibPng` | libpng 1.6.59(2026-09-28) | UE `UElibPNG` | 공식 배포 SHA-256·서명(도입 때 확인) | FreeType의 컬러 비트맵 글리프 PNG 해제용 | libpng-2.0 |
| `HarfBuzz` | HarfBuzz 15.2.0(2026-11-19) | 12.2.0 | GitHub release asset digest(SHA-256) | 합본 `src/harfbuzz.cc` + `src/hb-icu.cc`(별도 번역 단위) | Old MIT 계열 |
| `Icu` | ICU4C 79.1(2026-10-22, Unicode 18) | 78.1 | release asset digest + `.asc` 서명, `SHASUM512.txt` | `source/common`, `source/stubdata`, 데이터는 8.3절 | Unicode-3.0 |
| `Msdfgen` | msdfgen 1.13.1(2026-11-11) | 1.12(UE 단일 파일 헤더의 API 판 — 1.9절) | GitHub tag 원본(release asset digest 없음 — 커밋 해시 기록) | `core/` + `ext/import-font.cpp`(FreeType 윤곽 읽기만) | MIT |
| `NanoSvg` | nanosvg `239e102`(2026-07-09, 릴리스 없음) | UE 트리 판 | GitHub 커밋 해시 | `src/nanosvg.h`·`nanosvgrast.h` | Zlib |
| 기존 `Zlib`·`Brotli`(Core) | — | — | — | FreeType·libpng의 zlib, FreeType의 Brotli 의존을 이것으로 채운다(새로 들이지 않음) | — |

nanosvg는 글꼴이 아니라 UiToolkit SVG brush(4.1절)용이며 소비 모듈은 UiRender다(3장). FT1d에서 확인해 보니 원본 `#pragma warning(disable: 4996)`이 가리던 경고(`sscanf` 세 곳·`strncpy` 세 곳·`nsvgParseFromFile`의 `fopen`, 부호 비교 한 곳 — 지시문을 지운 사본도 경고 0이어야 한다)를 고치는 수정이 필요해, 도입은 SVG brush를 쓰는 UiToolkit UT2로 미룬다(2026-11-24). 그때 구현부는 glue C 번역 단위 하나(`NANOSVG_IMPLEMENTATION`·`NANOSVGRAST_IMPLEMENTATION`)로 빌드하는 code 라이브러리로 둔다.

### 8.2 빌드 구성

- **FreeType**
  - 번역 단위: `src/base/` 중 `ftsystem.c`·`ftinit.c`·`ftdebug.c`·`ftbase.c`·`ftbbox.c`·`ftglyph.c`·`ftbitmap.c`(ftglyph·ftsynth가 필요)·`ftstroke.c`(외곽선)·`ftsynth.c`(합성 굵게·기울임)·`ftmm.c`(변수 글꼴 기본 instance 처리), 드라이버·모듈 `truetype/truetype.c`·`cff/cff.c`·`psaux/psaux.c`·`pshinter/pshinter.c`·`psnames/psnames.c`·`sfnt/sfnt.c`·`autofit/autofit.c`·`smooth/smooth.c`·`raster/raster.c`·`gzip/ftgzip.c`.
  - 설정은 FreeType이 문서로 권하는 방식대로 `include/freetype/config/ftoption.h`·`ftmodule.h`를 고친다(추적 가능한 수정 — 소비 모듈도 같은 설정 헤더를 보므로 정의를 따로 전달할 필요가 없다). 모듈 목록: autofit·truetype·cff·psaux·psnames·pshinter·sfnt·smooth·raster1.
  - 켬: `FT_CONFIG_OPTION_USE_ZLIB` + `FT_CONFIG_OPTION_SYSTEM_ZLIB`(Core Zlib, 원본 내장 zlib 사본 대신), `FT_CONFIG_OPTION_USE_BROTLI`, `FT_CONFIG_OPTION_USE_PNG`, `FT_CONFIG_OPTION_ERROR_STRINGS`, `TT_CONFIG_OPTION_COLOR_LAYERS`(COLRv0), `TT_CONFIG_OPTION_GPOS_KERNING`(FD19).
  - 끔: `FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES`(환경 변수 `FREETYPE_PROPERTIES`로 동작이 바뀌면 결정성이 깨진다), bzip2·LZW, `FT_CONFIG_OPTION_MAC_FONTS`, FreeType 안의 HarfBuzz 연결(`FT_CONFIG_OPTION_USE_HARFBUZZ` — HarfBuzz가 FreeType에 의존하므로 순환), OT-SVG hook(`FT_CONFIG_OPTION_SVG`), Apple 실험 드라이버 hvf. 들이지 않는 드라이버: 비트맵 글꼴(bdf·pcf·winfnt·pfr)과 PostScript 윤곽 형식(type1·cid·type42) — 임포트가 받지 않는 형식이다. FreeType 자체 SDF 렌더러(`src/sdf`)도 msdfgen을 쓰므로 들이지 않는다.
  - Core Zlib은 `Z_SOLO`·`Z_PREFIX`를 `consumerDefines`로 요구한다(`Core/Source/ThirdParty/Zlib/ThirdParty.json`). FreeType·libpng가 Zlib에 의존하면 그 정의가 두 라이브러리 컴파일에 전달된다. `Z_SOLO`라 zlib 기본 할당기가 없으므로 FreeType `ftgzip.c`(FreeType 메모리로 `zalloc` 지정)와 libpng(자체 `zalloc`)가 할당 함수를 준다.
  - 할당: `FT_New_Library`에 Rocks 할당기를 건 `FT_Memory`로 library를 만든다(`FT_Init_FreeType`의 `malloc` 미사용).
- **HarfBuzz**: `harfbuzz.cc` + `hb-icu.cc`. `HAVE_FREETYPE`·`HAVE_ICU`·`HAVE_ICU_BUILTIN`(UE 12.2.0 빌드와 같다 — 1.9절, FD15). glib·cairo·graphite·coretext·directwrite·uniscribe 끔. 할당은 `hb_malloc_impl`·`hb_calloc_impl`·`hb_realloc_impl`·`hb_free_impl` 매크로를 Rocks 할당 함수로 정한다.
- **ICU**: `source/common` 전부와 `source/stubdata/stubdata.cpp`(`udata_setCommonData`로 데이터를 넣을 때 필요한 진입점 `U_ICUDATA_ENTRY_POINT`). 정의: `U_COMMON_IMPLEMENTATION`(없으면 `putil.cpp`가 `#error`), `U_STATIC_IMPLEMENTATION`, `U_CHARSET_IS_UTF8=1`, `UCONFIG_NO_LEGACY_CONVERSION=1`, `UCONFIG_NO_CONVERSION=1`·`UCONFIG_NO_IDNA=1`(엔진이 쓰지 않는 변환기·IDNA, ICU가 자기 사용 중단 API를 부르는 경로도 함께 빠진다), `UCONFIG_NO_FILE_IO=1`(데이터는 메모리로만), `U_USING_ICU_NAMESPACE=0`. `rtti: true`(1.9절·C++ 서드파티 항목). 경고 규칙(`-Werror`)에 걸리는 다섯 곳은 추적 가능한 수정으로 고쳤다: `cstring.h`의 `uprv_strcpy`·`strcat`·`strncpy`·`strncat`(같은 결과의 memcpy 구현), `ubrk.cpp` `ubrk_clone`(사용 중단 `ubrk_safeClone` 대신 같은 본문), `putil.cpp`(Windows `_get_timezone`·`_get_tzname`, `ICU_TIMEZONE_FILES_DIR` 환경 변수 읽지 않음), `umapfile.cpp`·`uinit.cpp`(쓰지 않는 인자 표시). 이름 바꾸기(`u_xxx_79`)는 켜 둔다(macOS 시스템 `libicucore`와 충돌 방지). 할당은 `u_setMemoryFunctions`로 Rocks 할당기. `globalState` = true(3장).
- **msdfgen**(FT1d 구현): 최상위 `msdfgen.h`와 `core/` 헤더 전부, `core/*.cpp` 중 파일 출력 도구(`export-svg`·`save-*`·`shape-description`)를 뺀 것 + `ext/import-font.cpp`. 미사용 인자·필드 초기화 경고 네 파일은 추적 가능한 수정. `import-font.h`는 FreeType 헤더 뒤에 include해야 FT_Face 함수가 선언된다(FreeType 윤곽 → `msdfgen::Shape`, `MSDFGEN_USE_FREETYPE` — UE `SlateSdfGenerator.cpp` 261행과 같은 경로). 나머지 ext(SVG·PNG 저장·tinyxml2·skia)는 들이지 않는다. `MSDFGEN_PUBLIC` 비움.
- **libpng**: `scripts/pnglibconf.h.prebuilt`를 그대로 쓴다(원본 파일). 아키텍처별 SIMD 경로(`PNG_*_OPT=0`)는 끈다 — PNG는 무손실이라 결과는 같고, 플랫폼별 소스 목록을 두지 않아 빌드가 단순해진다.
- **C++ 서드파티.** HarfBuzz·ICU·msdfgen은 C++이다. 빌드 체계는 원래 서드파티 code 라이브러리를 C17로만 받았으므로, C++ 번역 단위를 받도록 넓혔다([압축](../Compression.ko.md) 3.3절 "C++ 서드파티"). 엔진과 같은 C++23 인자로 컴파일하고 RTTI·예외는 끈다. ICU만 manifest `rtti: true`로 RTTI를 켠다(ICU4C common의 `dynamic_cast`·`typeid` — 사용자 결정 2026-11-24). ICU 소비자는 C API만 쓴다.
- 모든 라이브러리는 정적 라이브러리다. Rocks 소비 모듈은 FreeType·HarfBuzz·Icu·Msdfgen·LibPng는 `TextLayoutBackend`(ClientOnly)뿐이고, NanoSvg는 UiRender다(3장). 서드파티 사이 의존은 FreeType → {Zlib, Brotli, LibPng}, LibPng → Zlib, HarfBuzz → {FreeType, Icu}, Msdfgen → FreeType이다.
- **msdfgen glue `RocksApproximateSdf.cpp`**(FT2e): UE가 거리장 자리표시와 SdfApproximation에 쓰는 `approximateSDF`는 1.13.1 릴리스에 없고 원본 저장소의 `performance-optimization` 브랜치(커밋 `3af96cda22c2eee039a41ccef67adfdb2188d469`, `core/approximate-sdf.cpp`, 같은 저자·MIT)에만 있다. UE 사본 대신 원본 브랜치에서 가져와 glue로 둔다. 1.13.1에는 그 파일이 쓰는 `verticalScanlineIntersections`가 없어, 세로 pass는 x·y를 바꾼 shape 사본에 `scanlineIntersections`를 써서 같은 값을 얻는다(알고리즘·결과 불변, 이름 가림만 정리). 서드파티 glue는 번역 단위만 둘 수 있어 헤더가 없고, `TextLayoutBackend`가 같은 서명으로 선언한다(HarfBuzz `hb_*_impl` hook과 같은 방식 — 서명이 다르면 link가 실패한다). 1.13.1의 다음 판이 이 함수를 내면 glue를 지우고 원본으로 바꾼다.
- **STL**: msdfgen은 공개 형식(`msdfgen::Shape`)에 `std::vector`를 쓴다. Rocks 정책은 Rocks 소스에 쓴 `std` 이름과 STL include를 보므로, `TextLayoutBackend`는 msdfgen 멤버만 쓰고 `std` 이름을 쓰지 않아 예외가 필요 없다(FT2e 구현에서 확인 — 처음 계획한 좁은 예외는 등록하지 않았다). `std::priority_queue`를 쓰는 근사 함수는 ThirdParty glue에 있다. 다른 라이브러리는 C API만 쓴다. Rocks 정책 검사(STL·형식 규칙)는 원본 디렉터리에는 적용하지 않는다(기존 ThirdParty와 같다).

### 8.3 ICU 데이터

- ICU는 경계 규칙·사전(태국어·라오어·크메르어·버마어·CJK)을 데이터 파일에서 읽는다. 문자 속성(`uprops`·`ubidi`·`ucase`·`ulayout` 등)은 `common`에 내장되어 있어 데이터 파일이 필요 없다.
- 공식 `icu4c-79.1-data-bin-l.zip`(little-endian `.dat`, release digest·서명 확인)을 원본으로 들이고, 필요한 항목만 남긴 부분집합을 만든다: `brkitr/` 아래 규칙(char·word·line·sentence와 그 culture 변형)과 사전, 그리고 그것들이 참조하는 `brkitr` 로캘 리소스·`res_index`. 그 밖의 로캘 데이터·변환기·collation·IDNA는 뺀다.
- 부분집합은 ICU 공식 Windows 바이너리(`icu4c-79.1-Win64-MSVC2026.zip`, release digest 확인)의 `icupkg.exe`로 만든다: `icupkg -tl -r remove.txt icudt79l.dat <dir>/icudt79l.dat`(remove.txt = `icupkg -l` 전체에서 남길 항목을 뺀 목록). 남긴 항목은 39개다: `brkitr/` 36개(경계 규칙·로캘 리소스·태국어·라오어·크메르어·버마어·CJK 사전), `nfkc.nrm`(CJK 사전 엔진), `ulayout.icu`·`uemoji.icu`(데이터로 읽는 배치·이모지 속성). icupkg는 항목 이름 앞에 패키지 이름을 붙이므로 출력 파일 이름은 ICU가 찾는 `icudt79l.dat` 그대로여야 하고, 그러면 두 번 만들어도 byte가 같다.
- 결과(3,509,728 byte)는 데이터 라이브러리 `Engine/Source/ThirdParty/IcuData`(kind = data)에 생성 파일로 커밋하고, 입력 SHA-256·도구·명령·남긴 항목을 `ThirdParty.json`에 적는다(별도 도구 빌드 체계 대신 — 데이터 하나라 공식 바이너리로 한 번 만든다). 실행 파일 옆 배치(`Engine/Content/Internationalization`, UE와 같은 위치)와 `TextLayoutBackend` `Startup`의 `udata_setCommonData` 등록은 FT2에서 한다. 파일이 없거나 해시가 다르면 backend 등록을 실패시키고 오류를 남긴다(경계·BiDi가 없는 채로 돌지 않는다).
- 크기: 전체 데이터 33.6 MB에서 3.5 MB로 줄였다(사전이 대부분).

### 8.4 고지

라이브러리 고지 의무(FTL credit 문구, libpng·ICU·HarfBuzz·msdfgen·nanosvg 저작권 문구)는 `ThirdParty.json` `binaryNoticeRequired`로 표시하고, 기존 배포 고지 묶음(압축 2절 "고지 의무")에 함께 들어간다. 엔진 기본 글꼴·시험 글꼴 파일(SIL OFL 1.1·Apache-2.0)도 같은 묶음에 넣는다.

### 8.5 FT1 완료 조건

1. 여섯 모듈이 Windows에서 빌드되고, macOS는 CI에서 빌드된다. Linux는 지금 Server·Program 타깃만 빌드하므로(빌드 타깃 BD18, Linux 클라이언트는 BS6 뒤) Linux는 Program 타깃(시험 실행 파일)으로 확인한다.
2. 서버 제외는 빌드 규칙 로드 때의 BD25 의존 검사와 모듈 type 시험(`TextLayoutBackend`·글꼴 서드파티가 ClientOnly 도달 집합에만 있음)으로 확인한다. 실제 서버 타깃 링크 목록 검사는 서버 타깃이 생기는 BS7(네트워크 단계) 뒤다.
3. 연결 시험(EngineTests의 외부 라이브러리 시험 `Programs/FoundationTests/Engine/ThirdPartyFontLibraryTests.cpp` — 셰이더 도구 외부 라이브러리 시험과 같이 실행 파일이 라이브러리를 직접 링크하고 adapter 헤더를 경고 0으로 include한다. 엔진 Runtime 모듈은 FT2에서 만든다, CPU): FreeType 버전·고정 TTF 메모리 열기·glyph 하나 래스터, TTC 하위 face 수, 컬러 비트맵 글리프(PNG) 한 개 로드, WOFF1(zlib)·WOFF2(Brotli) 열기, HarfBuzz로 `"office"`의 `ffi` 합자와 아랍어 한 단어 shaping, ICU 줄 경계(한국어·영어·태국어 사전), ubidi로 혼합 방향 run, msdfgen으로 FreeType 윤곽 하나의 MTSDF, nanosvg로 작은 SVG 래스터. 결과는 golden과 대조한다.
4. 할당 hook 확인: FreeType(`FT_Memory`)은 할당이 Rocks 할당기로 가고 종료 때 남은 할당 0, HarfBuzz(`hb_*_impl` — HarfBuzz를 링크하는 모듈이 `RocksHarfBuzzMalloc` 등 네 함수를 정의한다. FT1에서는 시험 실행 파일이, FT2부터는 backend가 정의한다)는 hook으로 할당이 오는지. ICU(`u_setMemoryFunctions`)와 `u_cleanup` 정리는 프로세스 전역이라 시험 하나가 바꾸면 다른 시험(hb-icu가 ICU 정규화 객체를 붙잡는다)이 깨지므로, backend 시작(첫 ICU 사용 전 hook 설정·데이터 등록)·종료(HarfBuzz를 모두 놓은 뒤 `u_cleanup`) 경로와 함께 FT2에서 시험한다. **한계**: FreeType `pngshim.c`가 부르는 libpng 기본 할당(`png_create_read_struct`), FreeType `sfwoff2.c`의 `BrotliDecoderDecompress` 기본 할당, msdfgen의 `std::vector`(전역 `operator new`)는 원본을 고치지 않고는 Rocks 할당기로 돌릴 수 없다. 이 셋은 FT1에서는 표준 할당 경로로 남기고 `ThirdParty.json` notes에 적는다(추적 가능한 수정으로 FreeType 메모리를 넘기는 것은 가능하나 — `png_create_read_struct_2`·`BrotliDecoderCreateInstance` — 동작 변경이라 FT2에서 필요하면 따로 정한다).
5. 시험 글꼴(FT1e 구현)은 출처마다 데이터 라이브러리 하나다(`ThirdParty.json` kind = data, 라이선스 포함, 제품에 싣지 않음): `TestFontRoboto`(Roboto v3.016), `TestFontNanumGothic`(UE 편집기 한국어 typeface — Noto CJK는 원본이 수십~100 MB라 대신), `TestFontNotoNaskhArabic`(가변 글꼴, 기본 instance) — 모두 원본 byte 그대로. 원본이 너무 큰 입력(CBDT 컬러 이모지 10.7 MB)과 묶음·웹 형식은 `TestFontGenerated`의 **생성 파일**이다: U+1F600 한 글자 CBDT 부분집합, TTC(Roboto + Noto Naskh Arabic), Roboto의 WOFF·WOFF2. fontTools 4.63.0(brotli 1.2.0)으로 `recalcTimestamp=False`로 만들어 두 번 만들어도 byte가 같고, 도구 판·명령·입력 SHA-256을 manifest에 적었다. 연결 시험은 컬러 비트맵 글리프(strike 선택 → `FT_LOAD_COLOR` → libpng → BGRA), TTC 하위 face 둘, WOFF(zlib)·WOFF2(Brotli) 열기, 한글 래스터, 아랍어 RTL shaping(시각 순서·문맥형)을 본다.
6. format·policy 검사 통과, 바뀐 시험만 실행(전체 체인은 묶음 끝).
