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

nanosvg는 글꼴이 아니라 UiToolkit SVG brush(4.1절)용이며 소비 모듈은 UiRender다(3장). UD3의 같은 묶음이라 함께 들인다.

### 8.2 빌드 구성

- **FreeType**
  - 번역 단위: `src/base/` 중 `ftsystem.c`·`ftinit.c`·`ftdebug.c`·`ftbase.c`·`ftbbox.c`·`ftglyph.c`·`ftbitmap.c`(ftglyph·ftsynth가 필요)·`ftstroke.c`(외곽선)·`ftsynth.c`(합성 굵게·기울임)·`ftmm.c`(변수 글꼴 기본 instance 처리), 드라이버·모듈 `truetype/truetype.c`·`cff/cff.c`·`psaux/psaux.c`·`pshinter/pshinter.c`·`psnames/psnames.c`·`sfnt/sfnt.c`·`autofit/autofit.c`·`smooth/smooth.c`·`raster/raster.c`·`gzip/ftgzip.c`.
  - 설정은 FreeType이 문서로 권하는 방식대로 `include/freetype/config/ftoption.h`·`ftmodule.h`를 고친다(추적 가능한 수정 — 소비 모듈도 같은 설정 헤더를 보므로 정의를 따로 전달할 필요가 없다). 모듈 목록: autofit·truetype·cff·psaux·psnames·pshinter·sfnt·smooth·raster1.
  - 켬: `FT_CONFIG_OPTION_USE_ZLIB` + `FT_CONFIG_OPTION_SYSTEM_ZLIB`(Core Zlib, 원본 내장 zlib 사본 대신), `FT_CONFIG_OPTION_USE_BROTLI`, `FT_CONFIG_OPTION_USE_PNG`, `FT_CONFIG_OPTION_ERROR_STRINGS`, `TT_CONFIG_OPTION_COLOR_LAYERS`(COLRv0), `TT_CONFIG_OPTION_GPOS_KERNING`(FD19).
  - 끔: `FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES`(환경 변수 `FREETYPE_PROPERTIES`로 동작이 바뀌면 결정성이 깨진다), bzip2·LZW, `FT_CONFIG_OPTION_MAC_FONTS`, FreeType 안의 HarfBuzz 연결(`FT_CONFIG_OPTION_USE_HARFBUZZ` — HarfBuzz가 FreeType에 의존하므로 순환), OT-SVG hook(`FT_CONFIG_OPTION_SVG`), Apple 실험 드라이버 hvf. 들이지 않는 드라이버: 비트맵 글꼴(bdf·pcf·winfnt·pfr)과 PostScript 윤곽 형식(type1·cid·type42) — 임포트가 받지 않는 형식이다. FreeType 자체 SDF 렌더러(`src/sdf`)도 msdfgen을 쓰므로 들이지 않는다.
  - Core Zlib은 `Z_SOLO`·`Z_PREFIX`를 `consumerDefines`로 요구한다(`Core/Source/ThirdParty/Zlib/ThirdParty.json`). FreeType·libpng가 Zlib에 의존하면 그 정의가 두 라이브러리 컴파일에 전달된다. `Z_SOLO`라 zlib 기본 할당기가 없으므로 FreeType `ftgzip.c`(FreeType 메모리로 `zalloc` 지정)와 libpng(자체 `zalloc`)가 할당 함수를 준다.
  - 할당: `FT_New_Library`에 Rocks 할당기를 건 `FT_Memory`로 library를 만든다(`FT_Init_FreeType`의 `malloc` 미사용).
- **HarfBuzz**: `harfbuzz.cc` + `hb-icu.cc`. `HAVE_FREETYPE`·`HAVE_ICU`·`HAVE_ICU_BUILTIN`(UE 12.2.0 빌드와 같다 — 1.9절, FD15). glib·cairo·graphite·coretext·directwrite·uniscribe 끔. 할당은 `hb_malloc_impl`·`hb_calloc_impl`·`hb_realloc_impl`·`hb_free_impl` 매크로를 Rocks 할당 함수로 정한다.
- **ICU**: `source/common` 전부와 `source/stubdata/stubdata.cpp`(`udata_setCommonData`로 데이터를 넣을 때 필요한 진입점 `U_ICUDATA_ENTRY_POINT`). 정의: `U_COMMON_IMPLEMENTATION`(없으면 `putil.cpp`가 `#error`), `U_STATIC_IMPLEMENTATION`, `U_CHARSET_IS_UTF8=1`, `UCONFIG_NO_LEGACY_CONVERSION=1`, `UCONFIG_NO_FILE_IO=1`(데이터는 메모리로만). 이름 바꾸기(`u_xxx_79`)는 켜 둔다(macOS 시스템 `libicucore`와 충돌 방지). 할당은 `u_setMemoryFunctions`로 Rocks 할당기. `globalState` = true(3장).
- **msdfgen**: `core/*.cpp` + `ext/import-font.cpp`(FreeType 윤곽 → `msdfgen::Shape`, `MSDFGEN_USE_FREETYPE` — UE `SlateSdfGenerator.cpp` 261행과 같은 경로). 나머지 ext(SVG·PNG 저장·tinyxml2·skia)는 들이지 않는다. `MSDFGEN_PUBLIC` 비움.
- **libpng**: `scripts/pnglibconf.h.prebuilt`를 그대로 쓴다(원본 파일). 아키텍처별 SIMD 경로(`PNG_*_OPT=0`)는 끈다 — PNG는 무손실이라 결과는 같고, 플랫폼별 소스 목록을 두지 않아 빌드가 단순해진다.
- 모든 라이브러리는 정적 라이브러리다. Rocks 소비 모듈은 FreeType·HarfBuzz·Icu·Msdfgen·LibPng는 `TextLayoutBackend`(ClientOnly)뿐이고, NanoSvg는 UiRender다(3장). 서드파티 사이 의존은 FreeType → {Zlib, Brotli, LibPng}, LibPng → Zlib, HarfBuzz → {FreeType, Icu}, Msdfgen → FreeType이다.
- **STL**: msdfgen은 공개 형식(`msdfgen::Shape`)에 `std::vector`를 쓴다. `TextLayoutBackend`의 msdfgen adapter 번역 단위만 STL 정책의 좁은 예외로 등록한다(STL은 유불리 판단 뒤 최소 사용, policy에 좁은 예외로 등록하는 전체 규칙). 다른 라이브러리는 C API만 쓰므로 예외가 필요 없다. Rocks 정책 검사(STL·형식 규칙)는 원본 디렉터리에는 적용하지 않는다(기존 ThirdParty와 같다).

### 8.3 ICU 데이터

- ICU는 경계 규칙·사전(태국어·라오어·크메르어·버마어·CJK)을 데이터 파일에서 읽는다. 문자 속성(`uprops`·`ubidi`·`ucase`·`ulayout` 등)은 `common`에 내장되어 있어 데이터 파일이 필요 없다.
- 공식 `icu4c-79.1-data-bin-l.zip`(little-endian `.dat`, release digest·서명 확인)을 원본으로 들이고, 필요한 항목만 남긴 부분집합을 만든다: `brkitr/` 아래 규칙(char·word·line·sentence와 그 culture 변형)과 사전, 그리고 그것들이 참조하는 `brkitr` 로캘 리소스·`res_index`. 그 밖의 로캘 데이터·변환기·collation·IDNA는 뺀다.
- 부분집합은 ICU 도구 `icupkg`(호스트 도구, `source/tools/icupkg`와 `source/tools/toolutil`로 빌드)로 만든다. 남길 항목 목록은 Rocks 파일(`IcuDataItems.txt`)이며, 결과 `.dat`의 SHA-256을 `ThirdParty.json` notes에 적는다.
- 배치: 결과 `.dat`(`icudt79l_rocks.dat`)를 엔진 바이너리와 함께 배포하는 엔진 데이터 디렉터리 `Engine/Content/Internationalization/`에 둔다(UE `Engine/Content/Internationalization`과 같은 위치 — `icu4c-78_1/BuildForUE/README.txt`). 빌드 산출물 staging 규칙(FT1c에서 정함)이 바이너리 옆 같은 상대 경로로 복사한다. `TextLayoutBackend` `Startup`이 파일을 읽어 `udata_setCommonData`로 등록한다(`UCONFIG_NO_FILE_IO`). 파일이 없거나 해시가 다르면 backend 등록을 실패시키고 오류를 남긴다(경계·BiDi가 없는 채로 돌지 않는다).
- 크기 목표: 경계에 필요한 만큼(사전 포함 수 MB). 정확한 값은 FT1c 결과로 기록한다.

### 8.4 고지

라이브러리 고지 의무(FTL credit 문구, libpng·ICU·HarfBuzz·msdfgen·nanosvg 저작권 문구)는 `ThirdParty.json` `binaryNoticeRequired`로 표시하고, 기존 배포 고지 묶음(압축 2절 "고지 의무")에 함께 들어간다. 엔진 기본 글꼴·시험 글꼴 파일(SIL OFL 1.1·Apache-2.0)도 같은 묶음에 넣는다.

### 8.5 FT1 완료 조건

1. 여섯 모듈이 Windows에서 빌드되고, macOS는 CI에서 빌드된다. Linux는 지금 Server·Program 타깃만 빌드하므로(빌드 타깃 BD18, Linux 클라이언트는 BS6 뒤) Linux는 Program 타깃(시험 실행 파일)으로 확인한다.
2. 서버 제외는 빌드 규칙 로드 때의 BD25 의존 검사와 모듈 type 시험(`TextLayoutBackend`·글꼴 서드파티가 ClientOnly 도달 집합에만 있음)으로 확인한다. 실제 서버 타깃 링크 목록 검사는 서버 타깃이 생기는 BS7(네트워크 단계) 뒤다.
3. 연결 시험(EngineTests의 외부 라이브러리 시험 `Programs/FoundationTests/Engine/ThirdPartyFontLibraryTests.cpp` — 셰이더 도구 외부 라이브러리 시험과 같이 실행 파일이 라이브러리를 직접 링크하고 adapter 헤더를 경고 0으로 include한다. 엔진 Runtime 모듈은 FT2에서 만든다, CPU): FreeType 버전·고정 TTF 메모리 열기·glyph 하나 래스터, TTC 하위 face 수, 컬러 비트맵 글리프(PNG) 한 개 로드, WOFF1(zlib)·WOFF2(Brotli) 열기, HarfBuzz로 `"office"`의 `ffi` 합자와 아랍어 한 단어 shaping, ICU 줄 경계(한국어·영어·태국어 사전), ubidi로 혼합 방향 run, msdfgen으로 FreeType 윤곽 하나의 MTSDF, nanosvg로 작은 SVG 래스터. 결과는 golden과 대조한다.
4. 할당 hook 확인: FreeType(`FT_Memory`)·HarfBuzz(`hb_*_impl`)·ICU(`u_setMemoryFunctions`)의 할당이 모두 Rocks 할당기로 가고 종료 때 남은 할당 0. **한계**: FreeType `pngshim.c`가 부르는 libpng 기본 할당(`png_create_read_struct`), FreeType `sfwoff2.c`의 `BrotliDecoderDecompress` 기본 할당, msdfgen의 `std::vector`(전역 `operator new`)는 원본을 고치지 않고는 Rocks 할당기로 돌릴 수 없다. 이 셋은 FT1에서는 표준 할당 경로로 남기고 `ThirdParty.json` notes에 적는다(추적 가능한 수정으로 FreeType 메모리를 넘기는 것은 가능하나 — `png_create_read_struct_2`·`BrotliDecoderCreateInstance` — 동작 변경이라 FT2에서 필요하면 따로 정한다).
5. 시험 글꼴은 출처마다 데이터 라이브러리 하나(`Engine/Source/ThirdParty/TestFont<이름>`, `ThirdParty.json` kind = data, 라이선스 포함 — 예: `TestFontRoboto`)로 **원본 파일 그대로** 들인다(부분집합을 만들면 원본 byte 보존 규칙에 어긋난다): Roboto Regular·Bold, Noto Sans KR(가변이 아닌 정적 판 하나), Noto Naskh Arabic, Noto Color Emoji(CBDT 판), Noto Sans Thai, TTC 한 개(Noto Sans CJK의 `.ttc`). 원본 크기가 크면(CJK TTC·Color Emoji) 저장소 크기 영향을 FT1e에서 재고, 대안으로 OFL 글꼴을 공식 도구(fonttools `pyftsubset`)로 줄인 **생성 파일**을 쓴다면 도구·판·명령·입력 SHA-256을 notes에 기록한다.
6. format·policy 검사 통과, 바뀐 시험만 실행(전체 체인은 묶음 끝).
