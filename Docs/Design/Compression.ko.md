# 압축 헬퍼 — 코덱 선정·빌드 통합·동기/비동기 API

기준일: 2026-11-12. Godot `Compression`(core/io/compression.h)과 UE 5.6 `FCompression`을 참고해, 엔진·도구·게임이 함께
쓰는 압축 헬퍼를 Core 계층에 둔다. 요구 조건은 **성능**, **라이선스 문제 없음**, **포팅 문제 없음**이다. 이 문서는 네 관점
(라이선스·포팅, API 계약·악성 입력, 빌드·정책 통합, 비동기)의 병렬 교차검증을 거친 설계다. 구현·실행 검증 결과는
[구현 상태](../Implementation/FoundationStatus.ko.md)와 별도 검증 기록을 따른다.

## 1. 범위

| 1차 범위 | 2차 범위(9·10절) | 3차 범위(11절) | 4차 범위(11.6절) | 5차 범위(12절) | 6차 범위(13절·5절) | 7차 범위(14절) | 후속 |
|---|---|---|---|---|---|---|---|
| 형식을 고르는 일회성 압축·해제, 최악 압축 크기, 크기를 모르는 해제(상한 필수, `Array` 뒤에 붙임), 재사용 문맥(`Compressor`·`Decompressor`), worker 비동기 압축·해제(의존 작업 연결·협력적 취소), 모든 할당의 Rocks `Allocator` 경유, 결정적 출력 | 블록 단위 seek 가능 컨테이너(Godot `FileAccessCompressed`·UE `FCompressedBuffer` 대응)와 큰 버퍼의 블록 병렬 압축·해제, gzip·zstd 스트리밍 객체(역압) | zstd 사전 사용(압축·해제 사전 handle, 일회성·문맥·스트림·비동기·블록 컨테이너 연동, frame 사전 ID 검사) | zstd 사전 학습(fastCover)·finalize, `licenses` manifest | LZ4 frame 형식(`Lz4Frame`, `lz4` CLI 호환. legacy frame 제외): 일회성·문맥·비동기·스트리밍, content checksum·원본 크기 검사, upstream 결과와의 byte 비교 | Brotli 해제 전용 형식(`Brotli`, RFC 7932, WOFF2·`Content-Encoding: br`): 모든 해제 API(일회성·문맥·스트림·비동기), 압축 경로 Unsupported. AsyncFile 완료 → 해제 작업 연결(5절, 완료) | 형식 플러그인 등록(UE `ICompressionFormat` 대응, `CompressionCodec.h`): 이름·revision으로 식별하는 사용자 형식의 일회성·문맥·비동기·수준·bound, 사용 중 해제 Busy(사용권), 형식 이름·revision 조회(내장 포함) | LZ4·LZ4 frame·zlib 사전, Brotli 압축, 사용자 등록 형식의 스트림·사전 |

## 2. 코덱 선정

| 형식 | 라이브러리 | 라이선스(SPDX) | 선정 이유 |
|---|---|---|---|
| `Lz4`(raw block)·`Lz4Frame`(LZ4 frame, 12절) | LZ4 1.11.1(2026-11-10), `lib/`의 `lz4.[ch]`·`lz4hc.[ch]`·`lz4frame.[ch]`·`xxhash.[ch]`만 | BSD-2-Clause. 원문은 `lib/LICENSE`다. 저장소 루트 `LICENSE`는 "lib은 BSD-2, 나머지는 GPL-2.0-or-later" 안내문이며 CLI(`programs/`)는 들이지 않는다 | 해제 최고속. 1.11은 HC 확장 상태(`LZ4_STREAMHC_EXTSIZE`)로 수준 10–12도 할당 없이 동작하고, 1.10.0이 Android(non-Apple aarch64 clang)에서 껐던 빠른 해제 루프를 켠다. 1.11.0의 회귀는 폐기 API `LZ4_resetStreamHC()`에 국한되며 Rocks는 쓰지 않는다. frame 형식은 스트리밍·무결성 검사를 더하는 대신 해제가 raw block의 60%대다(XXH32 checksum, 12.5절) |
| `Zstd`(frame, RFC 8878) | zstd 1.5.7(2025-02-19, 최신 안정) | `BSD-3-Clause OR GPL-2.0-only` 중 **BSD-3-Clause 선택**. 부분집합 전 파일(xxhash 포함)이 이중 라이선스이며 특허 조항은 없다. 단 사전 생성기의 `divsufsort.[ch]`는 MIT(Yuta Mori)라 따로 고지한다(11.6절) | 압축률·속도 균형, `ZSTD_customMem`으로 할당기 주입, 원본 크기·체크섬 기록. upstream C 라이브러리의 알려진 CVE는 없다(검색된 CVE는 zstd-jni 것) |
| `Brotli`(RFC 7932, 해제 전용, 13절) | google/brotli 1.1.1(2026-10-21), 해제기(`c/common`·`c/dec`, 해제 헤더)만 | MIT. 원문은 `LICENSE`다 | WOFF2 글꼴·HTTP `Content-Encoding: br` 자산 읽기(Godot `MODE_BROTLI`도 해제 전용). 할당 함수 2개와 opaque로 할당기 주입. 1.1.1은 1.1.0 해제기의 범위 밖 쓰기(CVE-2026-111497)를 고친 patch release다 |
| `Deflate`(RFC 1951)·`Zlib`(RFC 1950)·`Gzip`(RFC 1952) | zlib 1.4(2026-11-04) | Zlib | 호환 형식(PNG·gzip·zip). `zalloc`·`zfree`·`opaque`로 할당기 주입, 스트림 API로 4GiB 이상과 취소를 처리. 1.4는 최신 유지와 UB·Android·Windows 이식성 수정이 근거다(보안 수정 대부분은 Rocks가 쓰지 않는 gz·inflateSync·Prime·inflateBack API) |

제외한 후보: Oodle(독점 라이선스), FastLZ(LZ4가 속도·압축률 모두 우위), libdeflate(MIT·Deflate 최고속이지만 스트리밍이
없고 할당 함수에 opaque 인자가 없다. Deflate가 hot path가 되면 adapter 뒤에서 교체 후보), zlib-ng(빠르지만 플랫폼별 SIMD
구성 생성이 필요해 포팅 부담), LZMA(현재 용도 없음), Brotli 압축기(`c/enc`, 쓰는 곳이 없어 해제기만 들인다).

**고지 의무.** BSD-2·BSD-3·MIT(Brotli)는 바이너리 배포 시 저작권 고지·조건·면책 문구를 문서나 함께 배포하는 자료에 재현해야 한다.
Zlib 라이선스는 바이너리 고지 의무가 없고 수정본은 수정본임을 밝혀야 한다. Rocks는 원본을 수정하지 않는다. 코덱은 첫
버전부터 엔진 바이너리에 들어가므로 **외부 배포(설치 SDK·게임 패키지·APK) 전에 고지 묶음이 반드시 포함되도록 배포 경로에
gate를 둔다.** 설치 SDK는 각 라이브러리 manifest `licenses`의 모든 원문을 함께 복사한다(3.2절).

## 3. 저장소와 빌드

### 3.1 배치

외부 원본은 Core가 쓰므로 Core 계층에 둔다(Core는 Engine을 참조하지 않는다). UE `Engine/Source/ThirdParty/<lib>/<version>/`과
같은 방식이다.

```text
Core/Source/ThirdParty/
  Lz4/   Lz4.Build.cs   ThirdParty.json   Lz4Unity.c   lz4-1.11.1/lib/{LICENSE, lz4.[ch], lz4hc.[ch], lz4frame.[ch], xxhash.[ch]}
  Zstd/  Zstd.Build.cs  ThirdParty.json   zstd-1.5.7/{LICENSE, lib/zstd.h, lib/zstd_errors.h, lib/zdict.h, lib/common/…, lib/compress/…,
                                                      lib/decompress/…, lib/dictBuilder/…}
  Zlib/  Zlib.Build.cs  ThirdParty.json   zlib-1.4/{LICENSE, adler32.c, crc32.[ch], deflate.[ch], inffast.[ch], inffixed.h,
                                                    inflate.[ch], inftrees.[ch], trees.[ch], zconf.h, zlib.h, zutil.[ch]}
  Brotli/ Brotli.Build.cs ThirdParty.json brotli-1.1.1/{LICENSE,
                                           c/common/{constants,context,dictionary,platform,transform}.[ch],
                                           c/common/{shared_dictionary.c, shared_dictionary_internal.h, version.h},
                                           c/dec/{bit_reader,huffman,state}.[ch], c/dec/{decode.c, prefix.h},
                                           c/include/brotli/{decode,port,shared_dictionary,types}.h}
Core/Source/Runtime/Compression/   Compression.Build.cs  Public/Compression/  Private/  Tests/
```

- 버전 디렉터리(`<lib>-<version>/`)는 릴리스 tarball의 파일을 **byte 그대로** 둔다. `.gitattributes`는 이 디렉터리를
  `-text`로, `.editorconfig`는 trim·final newline 규칙의 예외로 둔다(줄끝·공백 변환 방지).
- 컴파일하지 않는 파일은 넣지 않는다. zstd는 `legacy/ deprecated/`, `zstdmt_compress.c threading.c debug.c
  huf_decompress_amd64.S`를 뺀 27개 번역 단위(frame 압축·해제 22개와 사전 학습용 `dictBuilder/`의 `cover.c fastcover.c
  zdict.c divsufsort.c`·`common/pool.c` 5개, 헤더 `pool.h threading.h`도 넣는다. 11.6절), zlib은 `gz*.c compress.c uncompr.c
  infback.c`를 뺀 8개, LZ4는 `lz4file*`와 `lz4frame_static.h`(12.2절)를 뺀다. Brotli는 해제기 10개 번역 단위와 그 헤더만
  둔다(압축기 `c/enc`·`encode.h`, 도구 `c/tools`, binding·시험 자료 제외, 13.1절).
- LZ4는 Rocks 글루 `Lz4Unity.c`(`#include "lz4.c"` 다음 `#include "lz4hc.c"`) 한 번역 단위로 빌드한다. upstream이 지원하는
  방식(`LZ4_SRC_INCLUDED`)이며 이렇게 하면 `lz4hc.c`의 `-Wunused-function` 억제 분기가 비활성이다. `lz4frame.c`·`xxhash.c`는
  따로 컴파일한다(합본에 넣을 수 없는 이유는 12.2절).

### 3.2 manifest(`ThirdParty.json`) — 단일 출처

라이브러리마다 다음을 기록한다. 모듈 규칙과 정책 검사가 같은 파일을 읽는다(규칙에 목록을 중복하지 않는다).

| 필드 | 뜻 |
|---|---|
| `name`·`version`·`upstreamDirectory` | 이름·고정 버전·원본 디렉터리(라이브러리 폴더 기준) |
| `source` | 릴리스 URL, tarball SHA-256, 서명 검증 방법(zlib `.asc`, zstd 공식 `.sha256`) |
| `licenses` | 라이선스 배열(1개 이상). 항목마다 `spdx`(선택한 SPDX)·`upstreamExpression`(upstream 표현식)·`file`(원문 파일 경로)·`binaryNoticeRequired`(바이너리 고지 필요 여부)와 선택 필드 `licenseSource`(`url`·`sha256`, 원문이 원본 archive 밖에서 왔을 때) |
| `files` | 원본 디렉터리 안 모든 파일의 상대 경로 → SHA-256(byte 기준) |
| `translationUnits` | 컴파일할 파일(라이브러리 폴더 기준. 원본 파일 또는 Rocks 글루) |
| `includeDirectories` | adapter가 private으로 쓰는 include 경로 |
| `defines`·`platformDefines` | 라이브러리 빌드 정의(3.4) |
| `consumerDefines` | adapter가 헤더를 include할 때 같아야 하는 정의(`Z_PREFIX` 등) |
| `adapterHeaders` | 엔진 소유 번역 단위가 include하는 외부 헤더 |
| `pragmaDirectives` | 원본의 컴파일러 지시문 전체: `#pragma`(`once` 제외, MSVC 번호 형식 포함)·`_Pragma`·`__pragma`·경고/deprecation 억제 define. 줄 이음을 합친 논리 줄 기준(파일·시작 줄·원문). 3.6 참고 |
| `notes` | 실험 API 고정 사유, 설정 매크로 근거 등 |

라이선스(`licenses`) 규칙:

- 한 라이브러리에 라이선스가 다른 구성요소가 있으면 항목을 여럿 둔다. zstd는 본체 `BSD-3-Clause`(`zstd-1.5.7/LICENSE`)와
  사전 생성기의 `MIT`(libdivsufsort-lite, 원문이 머리 주석에 있는 `zstd-1.5.7/lib/dictBuilder/divsufsort.h`) 두 항목이다.
- 빈 배열, 항목의 모르는·누락 필드, 같은 `file`을 두 번 적은 항목(대소문자만 다른 경로 포함)은 manifest 오류다.
- 각 항목의 `file`은 라이브러리 폴더 기준 경로이고 `files`에 등록된 원본 파일이어야 한다(`third-party` 검사). 원문이 archive
  밖에서 왔으면(예: Unicode `license.txt`) 그 항목에 `licenseSource`를 적되 파일 자체는 원본 디렉터리에 두고 `files`에
  등록한다.
- 설치 SDK는 code 라이브러리 모듈을 담으면 모든 항목의 원문을, data 라이브러리는 `embeddedIn` 모듈을 담을 때만 복사한다.
  위치는 `Licenses/<Lib>/<원본 디렉터리 기준 경로>`다(예: `Licenses/Zstd/LICENSE`, `Licenses/Zstd/lib/dictBuilder/divsufsort.h`,
  `Licenses/Lz4/lib/LICENSE`). 파일 이름만 쓰지 않고 원본 안 경로를 유지하므로 이름이 같은 원문(최상위 LICENSE와 하위
  구성요소의 LICENSE)도 겹치지 않고, 어느 구성요소의 고지인지 경로로 드러난다.
- 이전 형식(단일 `license` 객체와 최상위 `licenseSource`)은 2026-11-12 합의로 폐기했고 읽지 않는다.

### 3.3 빌드 도구의 C 지원

- 확장자가 정확히 소문자 `.c`인 번역 단위만 C17로 컴파일한다. 경고·FP 축약 금지·구성 매크로·CRT·최적화·디버그 정보·ASan
  프로필은 C++와 같다.
  - Windows clang-cl: `/clang:-std=c17`과 `/TC`. C++ 전용 `/GR- /EHs-c- /Zc:__cplusplus /clang:-std=c++23 /TP`는 뺀다.
  - macOS·Android: compiler가 `clang++` driver이므로 소스 앞에 `-x c`를 둔다(없으면 C++로 처리하며 `-Werror`에서 실패).
    `-std=c17`을 쓰고 `-fno-rtti -fno-exceptions`는 뺀다.
- C++ 번역 단위의 인자는 이전과 byte 단위로 같다. action key가 인자를 포함하므로 바뀌면 모든 증분 빌드가 무효화된다.
  테스트가 C++ 인자 golden을 고정한다.
- 증분 빌드의 header 추적에 `.c`를 포함한다(`lz4hc.c`가 `lz4.c`를 include).
- AST·reflection 생성은 C++만 다룬다.
- ThirdParty 모듈은 `CodeOptimization.Always`다(Debug에서도 `-O2`). UE가 외부 라이브러리를 최적화 빌드로 쓰는 것과 같은
  이유이며 CRT·구성 매크로·NDEBUG는 구성을 따른다.
- 외부 include 경로는 전파하지 않는다. ThirdParty 모듈은 `PublicIncludeDirectories`가 비어 있고, 소비 모듈(`Compression`)이
  manifest의 `includeDirectories`와 `consumerDefines`를 private으로 받는다. 공개 헤더는 외부 헤더를 포함하지 않는다.

### 3.4 코덱 빌드 정의

공식 설정 지점만 쓰고 원본은 수정하지 않는다.

| 라이브러리 | 정의 | 이유 |
|---|---|---|
| LZ4 | `LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION` | lz4.c·lz4hc.c의 할당 API를 컴파일 단계에서 제거(남은 할당 경로는 컴파일 오류). adapter는 상태를 직접 할당한다. lz4frame.c는 자기 할당 매크로를 쓰므로 아래 정의로 막는다 |
| LZ4 | `LZ4_USER_MEMORY_FUNCTIONS` | lz4frame.c의 기본 할당 경로(사용자 할당기가 없을 때)를 `Lz4Unity.c`의 `LZ4_malloc`·`LZ4_calloc`·`LZ4_free`로 보낸다. 이 함수들은 NULL을 돌려주므로 그 경로를 타면 CRT 대신 할당 실패가 된다(12.2절). lz4.c는 위 정의 분기가 먼저라 영향이 없다 |
| LZ4 | `LZ4LIB_VISIBILITY=`, `LZ4FLIB_VISIBILITY=` | ELF·Mach-O에서 `visibility("default")` 강제 해제(Android `.so` 심볼 노출 방지). 뒤의 것은 `lz4frame.h` |
| LZ4 | `XXH_NAMESPACE=LZ4_` | LZ4가 들고 있는 xxHash 0.6.5 심볼을 `LZ4_XXH32` 등으로 바꾼다. zstd는 자기 `xxhash.h`에서 `ZSTD_` 접두사를 정하므로 함께 링크해도 겹치지 않는다(12.2절) |
| zstd | `ZSTD_DISABLE_ASM=1` | `.S` 없이 모든 플랫폼 같은 C 경로. 잃는 것은 Android x86_64의 asm Huffman 해제뿐 |
| zstd | `ZSTD_TRACE=0`, 멀티스레드·legacy 미정의 | ELF weak symbol trace 비활성, 단일 스레드, 구형 형식 제외 |
| zstd | `ZSTDLIB_VISIBLE=`, `ZSTDERRORLIB_VISIBLE=`, `ZDICTLIB_VISIBLE=` | 심볼 노출 해제(위와 같음). `ZDICTLIB_STATIC_API`는 DLL 정의가 없으면 `ZDICTLIB_VISIBLE`로 정해지므로 따로 주지 않는다 |
| zstd | `MEM_STATIC=static __inline __attribute__((unused))` | clang-cl은 `__GNUC__`가 없어 zstd가 GNU 계열에서 기본으로 붙이는 `unused` 속성을 빼므로, assert 전용 `HUF_isSorted`·`ZSTD_isAligned`가 `-Wunused-function` 오류가 된다. zstd의 `#ifndef MEM_STATIC` 가드(커널 빌드용, README 설정 항목은 아님)로 GNU 계열 정의를 그대로 재현한다. 경고 정책 "외부 원본 내부 진단 지시문" 4항으로 승인 |
| zstd(Windows) | `DYNAMIC_BMI2=0` | clang-cl에서는 `TARGET_ATTRIBUTE`가 비어 BMI2 사본이 이득 없이 중복된다(README 문서화 설정). 해제 속도 측정으로 확인 |
| zlib | `Z_SOLO` | 기본 할당기·gz 파일 IO 제거(할당기 주입 강제, `_CRT_SECURE_NO_WARNINGS`를 정의하는 `gzguts.h` 미포함). `memcpy`가 내부 루프로 바뀌지만 `-O2`에서 벡터화 복사로 컴파일됨을 확인 |
| zlib | `Z_U4=__UINT32_TYPE__`, `Z_U8=__UINT64_TYPE__` | `Z_SOLO`가 끄는 braid CRC-32·ARMv8 하드웨어 CRC를 되살린다(Gzip 성능) |
| zlib | `Z_PREFIX` | 심볼에 `z_` 접두사(Android·macOS 시스템 libz와 충돌 방지) |
| zlib(Windows 외) | `HAVE_HIDDEN` | 내부 심볼 hidden |
| Brotli | (없음) | `BROTLI_SHARED_COMPILATION`을 정의하지 않으면 `BROTLI_DEC_API`·`BROTLI_COMMON_API`가 비고 내부 심볼(`BROTLI_INTERNAL`)은 GNU 계열에서 hidden이다(LZ4·zstd의 visibility 정의를 비운 것과 같다). 외부 심볼은 `Brotli`·`kBrotli`·`_kBrotli` 접두사라 다른 코덱과 겹치지 않는다(llvm-nm). 할당 기본 경로를 막는 설정 지점은 없다(13.2절). consumerDefines도 없다 |

`Z_SOLO`·`Z_PREFIX`·`Z_U4`·`Z_U8`, zstd의 visibility 정의와 `ZSTD_STATIC_LINKING_ONLY`·`ZDICT_STATIC_LINKING_ONLY`(fastCover 학습 API), LZ4의 `LZ4_HC_STATIC_LINKING_ONLY`·
`LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION`·`LZ4LIB_VISIBILITY=`·`LZ4F_STATIC_LINKING_ONLY`(사용자 할당기 frame 문맥)·
`LZ4FLIB_VISIBILITY=`는 adapter에도 같은 값으로 준다(`consumerDefines`). `lz4frame.c`는 `LZ4F_STATIC_LINKING_ONLY`를 스스로
정의하므로 라이브러리 정의에는 넣지 않는다.
같은 이름이 라이브러리 정의에도 있으면 값이 같아야 한다(manifest 리더가 확인). `LZ4_STATIC_LINKING_ONLY`는 `lz4hc.h`가
`LZ4_HC_STATIC_LINKING_ONLY`일 때 스스로 정의하므로 명령줄로 주지 않는다(값이 다르면 재정의 경고). LZ4 HC 확장 상태 API는
1.11의 실험 API이므로 manifest `notes`에 고정 사유를 남긴다.

### 3.5 정책 검사

- 기존 lexical 검사(서식, 경고 억제, 플랫폼 경계, 강제 인라인 금지)는 **manifest의 `upstreamDirectory`와 정확히 같은
  디렉터리만** 제외한다. 이름이 비슷한 디렉터리는 계속 검사한다. Rocks 글루(`*.Build.cs`, `Lz4Unity.c`)는 검사 대상이며
  검사 대상 확장자에 `.c`를 더한다.
- 새 `third-party` 검사: manifest 스키마 엄격 검사(모르는·중복 필드 거부), 실제 파일 집합과 SHA-256(byte 기준) 일치,
  링크 파일 거부, `licenses`의 모든 원문이 등록된 원본 파일임, `translationUnits` ⊆ 원본 파일 ∪ 글루, 컴파일러 지시문
  목록(`pragmaDirectives`)의 정확한 일치.
- adapter 헤더의 진단 억제는 같은 계열(MSVC `warning`, clang/GCC `diagnostic`)의 push/pop 안에서만 허용하고(pack 등 무관한
  push/pop은 세지 않는다), adapter 헤더가 따옴표로 include하는 원본도 adapter 헤더여야 한다. 엔진 소유 소스는 adapter 헤더
  외의 원본을 include할 수 없다(등록된 글루는 자기 라이브러리의 원본 `.c`를 include할 수 있다).
- 엔진 소유 소스의 억제 검사는 매크로 형태(`_Pragma("clang diagnostic ignored …")`, `__pragma(warning(disable …))`)도 잡는다.
- compile DB 감사: 번역 단위 언어와 std를 컴파일러처럼 마지막 지정 기준으로 해석해 확장자와 일치하는지 확인하고(`/TC`·`/TP`·
  `-x c`·`-xc`·`--language`), `.c`는 ThirdParty 라이브러리 폴더 아래에서만 허용한다. 억제·강등 금지와 필수 경고 플래그는
  C에도 같다. 모듈의 `AdditionalCompileArguments`로 언어·표준을 바꾸는 인자는 빌드 입력 검증이 거부한다.
- 업그레이드 도구: `Rocks.ToolTests third-party --root . [--write]`가 원본에서 `files`·`pragmaDirectives`를 다시 계산한다
  (검사와 같은 스캐너). 나머지 필드는 사람이 고친다.
- 음성 대조: 원본 1byte 변경, 미기재·누락 파일, LICENSE 삭제, manifest 밖 번역 단위, 새 pragma, CRLF 변환, 이름이 비슷한
  디렉터리, 글루의 pragma·강제 인라인, C 항목의 `-Wno-…`·`/TC` 누락·`.c`+`/TP`·ThirdParty 밖 `.c`.

### 3.6 외부 원본 내부의 진단 지시문 (2026-11-12 승인, [경고 정책](CompilerDiagnostics.ko.md)에 반영)

원본에는 다음 진단 지시문이 있다: zstd `compiler.h`(MSVC 4100·4127·4204·4214·4324), `xxhash.h`(4127, once 4606),
`huf_compress.c`(4127), LZ4 `lz4.c`(4127·6237·6239·6240·6326, push/6385/pop)·`lz4hc.c`(GCC·clang `-Wunused-function`, 합본에서
비활성)·`lz4frame.c`(4127)·`xxhash.c`(4127), zlib `deflate.c`(push/4127/pop),
`zstd.h`(clang push/`-Wzero-as-null-pointer-constant`/pop). Brotli `platform.h`의 `_Pragma` 5개는 TI·IAR·PGI용 인라인
지시라 진단 지시문이 아니다(manifest에는 기록하고, `native`의 지시문 무효 확인은 진단 지시문이 없어 Brotli를 건너뛴다).
**clang-cl은 일부 MSVC 번호를 clang 진단에 매핑해 실제로
따른다**(4100 → `-Wunused-parameter` 확인). 전체 목록은 각 manifest의 `pragmaDirectives`가 기준이다. 그래서 원본의 진단 지시문을 모두 지운 사본을 같은 프로필로 컴파일했다. 결과는
clang-cl `/W4 /WX`(`/Od`·`/O2`)와 GNU식 `-Wall -Wextra -Wpedantic -Werror`(`-O0`·`-O2`) 모두 경고 0이었다. 즉 이
버전·프로필에서 실제로 가리는 경고는 없다.

[경고 정책](CompilerDiagnostics.ko.md)에 다음 규칙을 추가했다(원문은 정책 문서가 기준이다).

> **외부 원본 내부 진단 지시문.** `ThirdParty/<Lib>/<upstreamDirectory>/`에 릴리스 원본을 byte 그대로 둔 경우에 한해,
> 원본에 이미 있는 진단 지시문은 다음을 모두 만족할 때 둔다. 이는 경고 억제 예외가 아니다.
> (1) 파일·줄·원문을 `ThirdParty.json`에 빠짐없이 기록하고 `third-party` 검사가 정확히 일치하는지 확인한다.
> (2) 지원 toolchain·구성마다 지시문을 제거한 사본도 경고 0임을 `Rocks.ToolTests native`가 매 실행 확인한다. 실제로 경고를
> 가리면 호환 버전 선택이나 추적 가능한 수정으로 해결한다.
> (3) 엔진 소유 번역 단위가 include하는 외부 헤더의 지시문은 그 헤더 안에서 push/pop으로 닫힌다.
> (4) 엔진·글루 소스·규칙·컴파일 인자로 억제를 추가하지 않는다. 단, upstream이 GNU 계열에서 기본으로 쓰는 속성을 공식
> 가드 매크로로 다른 컴파일러에 재현하는 경우(zstd `MEM_STATIC`)는 manifest에 사유와 함께 기록한다.

대안은 원본에서 지시문을 지우는 추적 가능한 수정이다. 이 경우 원본 byte 동일성이 깨지고, Zlib 라이선스에 따라 수정한
파일에 수정본임을 표시해야 하며, 업그레이드마다 같은 수정을 다시 적용해야 한다.

## 4. 동기 API — `Compression/Compression.h`

모듈 `Compression`(Core 계층, `Core/Source/Runtime/Compression`)이다. RequiredModules는 `Core`·`Task`와 ThirdParty `Lz4`·
`Zstd`·`Zlib`·`Brotli`다. Core 모듈에 외부 의존이 생기지 않고, 비동기 API가 Task를 쓸 수 있다. 정적 링크이므로 동기 API만 쓰는
모듈(예: AssetTools PNG)은 Task의 object를 링크하지 않는다.

```cpp
namespace rocks {

// 프로세스 안 식별용이다. 영속 포맷에 기록할 때는 상위 포맷이 별도 ID(또는 형식 이름)를 정한다. 0–255는 내장 형식이며
// 새 형식은 끝에 덧붙인다. 256 이상은 사용자 등록 형식(14절)의 값으로, 등록 순서에 따라 실행마다 다르다.
enum class CompressionFormat : uint32 { Lz4, Zstd, Deflate, Zlib, Gzip, Lz4Frame, Brotli }; // Brotli는 해제 전용(13절)

struct CompressionLevelRange {
    int32 minimum;
    int32 maximum;
    int32 defaultLevel;
};

// Lz4·Lz4Frame {1, 12, 1}: 1 = fast(acceleration 1), 2 = lz4mid, 3–9 = HC, 10–12 = HC optimal parser.
// Zstd {1, 22, 3}(20–22 ultra). Deflate·Zlib·Gzip {0, 9, 6}(0 = stored). 사용자 등록 형식은 등록한 범위.
// 모르는 형식은 InvalidArgument, 해제 전용 형식(Brotli)은 Unsupported.
Result<CompressionLevelRange> TryGetCompressionLevelRange(CompressionFormat format) noexcept;

// 형식 이름·revision이다. 내장 형식은 "lz4"·"zstd"·"deflate"·"zlib"·"gzip"·"lz4frame"·"brotli"와
// CompressionOutputRevision, 사용자 등록 형식은 등록 이름("oodle.kraken"처럼 '.' 포함)과 version이다. 빌드 캐시 키는
// 형식 값 대신 이름·revision과 수준·사전 Digest를 쓴다.
struct CompressionFormatInfo { /* Name()·revision·plugin·compresses */ };
Result<CompressionFormatInfo> TryGetCompressionFormatInfo(CompressionFormat format) noexcept; // 모름 InvalidArgument
Result<CompressionFormat> TryFindCompressionFormat(StringView name) noexcept; // 문법 위반 InvalidArgument, 없음 NotFound

// 검증된 형식·수준 쌍이다.
class CompressionSettings {
public:
    static Result<CompressionSettings> TryMake(CompressionFormat format, int32 level) noexcept;
    static Result<CompressionSettings> TryDefault(CompressionFormat format) noexcept;
    CompressionFormat Format() const noexcept;
    int32 Level() const noexcept;
};

inline constexpr usize Lz4MaxInputBytes = 0x7E000000; // LZ4 블록 형식의 입력 한계(LZ4_MAX_INPUT_SIZE)

Result<usize> TryGetCompressBound(CompressionFormat format, usize sourceBytes) noexcept;

Result<usize> TryCompress(const CompressionSettings& settings, Span<const uint8> source, Span<uint8> destination,
                          Allocator& scratch) noexcept;
Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source, Array<uint8>& output) noexcept;
Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source, Array<uint8>& output,
                                Allocator& scratch) noexcept;

Result<usize> TryDecompress(CompressionFormat format, Span<const uint8> source, Span<uint8> destination,
                            Allocator& scratch) noexcept;
Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source, usize maxOutputBytes,
                                  Array<uint8>& output) noexcept;
Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source, usize maxOutputBytes,
                                  Array<uint8>& output, Allocator& scratch) noexcept;

// 반복 호출용. 코덱 문맥을 한 번 할당해 재사용한다. 이동 전용이며 문맥은 heap에 고정한다(zlib state가 z_stream
// 주소를 검사하므로 값으로 두면 이동 후 깨진다). 한 객체는 동시에 한 스레드만 쓴다.
class Compressor {
public:
    static Result<Compressor> TryCreate(const CompressionSettings& settings, Allocator& allocator) noexcept;
    Result<usize> TryCompress(Span<const uint8> source, Span<uint8> destination) noexcept;
    Result<usize> TryCompressAppend(Span<const uint8> source, Array<uint8>& output) noexcept;
};

class Decompressor {
public:
    static Result<Decompressor> TryCreate(CompressionFormat format, Allocator& allocator) noexcept;
    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination) noexcept;
    Result<usize> TryDecompressAppend(Span<const uint8> source, usize maxOutputBytes, Array<uint8>& output) noexcept;
};

} // namespace rocks
```

### 4.1 공통 계약

- **반환값**: 압축·해제 함수는 쓴(붙인) byte 수를 반환한다.
- **실패 시 출력**: Span 출력은 실패 시 내용이 미정이며 범위 밖에는 쓰지 않는다. Append는 실패 시 `output` 길이를 호출
  전으로 복원한다(capacity는 늘 수 있다).
- **작업 메모리**: scratch를 받지 않는 Append는 `output.GetAllocator()`를 쓴다. 호출 안에서 할당한 작업 메모리는 반환
  전에 모두 돌려준다(재사용 문맥 제외).
- **겹침**: source가 destination과 겹치거나, Append에서 source가 `output`의 `[Data, Data + Capacity)` 안에 있으면
  InvalidArgument다. 성장 중 재할당으로 dangling이 되는 것을 막는다.
- **빈 입력·크기 산술**: 빈 Span의 nullptr는 adapter가 더미 주소로 바꿔 넘긴다(zlib은 `next_out == NULL`을 거부). 해제에
  빈 source는 InvalidArgument다. `maxOutputBytes == 0`은 결과가 비어야 성공이다. 기존 길이 + 상한의 overflow는 Overflow다.
- **스레드**: 자유 함수는 재진입 가능하며 전역·thread_local 캐시가 없다. Godot의 thread_local zstd 문맥 대신 명시적
  재사용 문맥으로 성능을 얻는다. 설정 성격의 전역 상태는 사용자 등록 형식 표(14.3절)뿐이며 내장 형식 경로는 이 표를 읽지
  않는다. 그 밖에는 결과에 영향을 주지 않는 내부 전역(빈 Span 대체 주소, 블록 컨테이너 reader 번호 카운터)만 있다.
- **결정성**: 같은 라이브러리 버전·설정·입력이면 64bit little-endian 대상(Windows x64·macOS ARM64·Android arm64/x86_64)
  에서 같은 byte다. LZ4는 32bit 대상에서 hash·table 형식이 달라 결정성 범위 밖이다.
  - zlib: windowBits 15·memLevel 8·`Z_DEFAULT_STRATEGY`로 고정한다. Gzip 헤더는 매 reset 뒤 `deflateSetHeader`로
    mtime 0·OS 255·이름·주석·extra·FHCRC 없음을 다시 적용한다(zlib 1.4 `deflateResetKeep`이 헤더를 지우고, 기본 OS 코드는
    플랫폼마다 다르다).
  - zstd: checksum과 content size를 켠다. 사전 ID는 사전 문맥만 기록한다(11절).
  - LZ4 frame: 12.1절 설정을 고정한다. 일회성 결과는 64KiB를 넘는 입력에서 upstream `LZ4F_compressFrame`과 같은 byte다.
  - 동기·비동기는 같은 내부 구현과 같은 chunk 상수를 쓴다.
  - 테스트가 고정 입력의 압축 결과 CRC-32를 golden으로 고정한다. 버전을 올릴 때는 의도적으로 갱신한다.
  - 사용자 등록 형식의 결정성은 코덱 책임이며, 출력이 바뀌는 변경마다 등록 version(revision)을 올린다(14.2절).
- **무결성**: Zstd는 content checksum(XXH64 하위 32bit), Zlib은 Adler-32, Gzip은 CRC-32·ISIZE, Lz4Frame은 content
  checksum(XXH32)·기록된 원본 크기·(있으면) 블록 checksum을 검사한다. Lz4 raw block·
  Deflate raw·Brotli는 무결성 검사가 없어 손상이 "성공한 다른 데이터"로 보일 수 있다. 체크섬 형식도 의미 없는 bit(zstd 프레임
  헤더의 미사용 bit, deflate 마지막 byte의 padding, FHCRC 없는 gzip의 MTIME·XFL·OS)는 바꿔도 같은 결과로 성공할 수 있다.
  따라서 계약은 "손상은 실패하거나, 성공하면 원본과 같은 출력"이다.

### 4.2 형식별 계약과 오류 대응

| 상황 | 오류 |
|---|---|
| destination 또는 `maxOutputBytes` 부족 | CapacityExceeded |
| 손상·절단·체크섬 불일치·스트림 뒤 남는 byte(Lz4 제외)·빈 해제 입력·범위 밖 수준·겹침·Lz4 입력 한계 초과, 모르는 형식 값(등록되지 않았거나 해제가 시작된 사용자 형식 값 포함) | InvalidArgument |
| 문맥에 없거나 다른 사전이 필요한 입력: zlib 프리셋 사전(FDICT, `Z_NEED_DICT`), 사전 ID가 다른 zstd frame(`dictionary_wrong`), 형식 사전 문맥에 사전 ID 없는 frame, 사전 ID를 기록한 LZ4 frame, 블록 컨테이너의 사전이 후보에 없음 | Unsupported |
| zstd frame의 window가 구현 상한(2^31)을 넘음(RFC상 유효, 일회성·스트림·사전 ID 조회 모두) | Unsupported |
| 해제 전용 형식(Brotli)의 압축 API(수준 범위·`CompressionSettings`·bound·압축 문맥), large window 확장 Brotli stream(비표준, 첫 byte 0x11) | Unsupported |
| 사용자 등록 형식의 스트림·사전 API(블록 컨테이너는 영속 codec ID라 InvalidArgument) | Unsupported |
| 할당 실패(`Z_MEM_ERROR`·`memory_allocation` 포함) | OutOfMemory |
| 크기 산술 overflow, `deflateBound_z`의 `(z_size_t)-1` | Overflow |
| 외부 코덱이 계약 밖의 실패를 보고(예: dst ≥ bound인데 LZ4 압축이 0 반환, `Z_STREAM_ERROR`), 사용자 등록 코덱이 반환 계약(14.2절)을 어김 | DependencyFailed |

- **Lz4**
  - 압축: fast(수준 1)는 `LZ4_initStream` 후 `LZ4_compress_fast_extState_fastReset`을 쓴다. HC(2–12)는
    `LZ4_STREAMHC_EXTSIZE` 버퍼를 `LZ4_initStreamHC`로 한 번 초기화한 뒤 `LZ4_compress_HC_extStateHC_fastReset`을 쓴다(상태
    8byte 정렬). 0 반환은 dst < bound면 CapacityExceeded, 아니면 DependencyFailed다.
  - 해제: `LZ4_decompress_safe`의 -1과 모르는 음수는 InvalidArgument다. -2("출력 부족 또는 손상")는 용량이 LZ4 최대 확장비
    (255 × 입력 + 상수)에 이르렀으면 InvalidArgument, 아니면 CapacityExceeded다. raw block에는 끝 표지가 없으므로 "남는
    byte" 검사는 하지 않는다.
  - int 경계: source가 `Lz4MaxInputBytes`를 넘으면 InvalidArgument, destination 용량은 `INT_MAX`로 자른다.
- **Lz4Frame**: 12.3절(일회성)·12.4절(스트림). 64KiB 블록으로 나누므로 입력 한계가 없다.
- **Brotli**: 해제 전용, 13.3절(일회성)·13.4절(스트림). stream은 이어 붙일 수 없어 뒤 byte는 InvalidArgument다.
- **사용자 등록 형식**: 14절. 크기를 모르는 해제는 모듈이 두 배 재시도로 처리한다(14.4절).
- **Zstd**
  - 압축: `ZSTD_CCtx`에 수준·checksum·content size를 고정한다. pledged size와 `ZSTD_c_stableInBuffer`를 쓰고 입력을 일정
    chunk씩 늘려 주는 스트림 루프로 처리한다(비동기 취소 지점, 동기와 같은 구현).
  - 해제: window를 할당하지 않는다. `ZSTD_decompressBound`(크기 미기록 frame은 블록 수 × 128KiB)로 `min(bound, 상한)`을 한
    번에 확보한다. 그다음 `ZSTD_d_stableOutBuffer`와 입력 chunk로 `ZSTD_decompressStream`을 호출한다. `ZSTD_d_windowLogMax`는
    최대로 두어 유효한 큰 window frame도 받는다(stable output이므로 window 메모리가 없다). 연속 frame·skippable frame은
    표준대로 처리하고, frame 뒤의 쓰레기는 InvalidArgument다.
  - `dstSize_tooSmall`: 모든 frame에 크기가 기록되어 있고 합이 용량 이하면 거짓 헤더이므로 InvalidArgument다. 합이 용량을
    넘으면 해제 전에 CapacityExceeded다. 크기 미기록 frame에서는 CapacityExceeded다.
- **Deflate·Zlib·Gzip**
  - z_stream과 `gz_header`는 할당기로 할당한 heap 블록에 고정한다.
  - bound: 공개 `TryGetCompressBound`는 `deflateBound_z(Z_NULL, n)`(1.4, `z_size_t`는 64bit 대상에서 64bit)으로 모든
    래퍼를 덮는 보수적 상한(약 13% 여유)을 준다. 압축 문맥이 있는 Append는 문맥 설정으로 구한 `deflateBound_z(&stream, n)`
    (약 0.03% 여유, 수준 0은 stored 기준)만큼만 확보한다. `Z_NO_FLUSH`·`Z_FINISH`만 쓰므로 분할 호출에도 성립한다.
  - 32bit 한계: `uInt` 한계를 넘는 입력·출력은 분할 단위(1MiB)로 나눠 넣는다. 1MiB를 넘는 일반 입력으로 같은 루프가
    실행되므로 별도 주입 없이 검증된다.
  - Append 해제: 출력을 `Array::TryResize`로 두 배씩(최소 64KiB) 늘리며 그 뒤로 직접 inflate한다(늘린 구간은 0으로
    채워진다). 실패하면 길이를 되돌린다.
  - 출력 상한 판별: 쓸 공간이 0이 되면(Span 끝 또는 Append 상한) 1byte 예비 버퍼로 계속 inflate한다. 1byte라도 나오면
    CapacityExceeded, 블록 끝·체크섬만 남았으면 성공이다(이 상태들은 출력 공간 없이 진행된다). 입력을 다 넣었는데 진행이
    없으면(`Z_BUF_ERROR`) 절단(InvalidArgument)이다. 그래서 정확히 상한 크기인 결과는 성공한다.
  - Gzip 연속 member: RFC 1952대로 모두 해제한다(`inflateReset`). member 사이·뒤의 쓰레기와 0 padding은 거부한다.
  - Gzip 헤더: FEXTRA·FNAME·FCOMMENT·FHCRC를 받고, 틀린 FHCRC와 예약 FLG는 거부한다.
  - zlib 1.4부터 raw deflate가 출력 버퍼를 정확히 채우면 `Z_STREAM_END`를 반환한다. 이를 정상으로 처리한다.
- **수준과 메모리**: zstd 수준 19의 압축 문맥은 큰 입력에서 수십 MiB를 쓰고(ultra 20–22는 큰 입력·크기를 모르는 스트림에서 수백 MiB), LZ4 HC 확장
  상태는 약 320KiB, zlib deflate는 약 256KiB, inflate는 약 40KiB, zstd 해제 문맥은 약 160KiB다. LZ4 frame 압축 문맥은 수준
  1 약 145KiB·HC 약 450KiB, 해제 문맥은 약 256KiB다(12.3절). Brotli 해제는 상태 약 7KiB + 출력에 맞춰 늘어나는 ring buffer
  (window 이하) + Huffman 표(최악 약 2.6MiB)이고, ring buffer를 늘리는 순간 최고 약 1.5 × window + 표다(13.3절). 정확한 값은
  테스트의 계수 allocator로 기록한다.

## 5. 비동기 API — `Compression/AsyncCompression.h`

```cpp
namespace rocks {

// 비동기 작업 한 건의 결과 보관소다. caller가 소유하고 이동할 수 없다(고정 주소). 결과 slot은 객체 안에 있다.
// 소멸자는 진행 중이면 RequestCancel 후 Wait한다(AsyncIoRequest와 같은 선례). 같은 scheduler worker에서 소멸해
// Wait가 WrongThread면 Fatal이다. source·output·scratch보다 먼저 소멸하도록 나중에 선언한다.
class CompressionTask {
public:
    CompressionTask() noexcept = default;
    ~CompressionTask() noexcept;
    CompressionTask(const CompressionTask&) = delete;
    CompressionTask& operator=(const CompressionTask&) = delete;

    // 의존 연결(Span(&task.Handle(), 1))·Wait·RequestCancel·Clone용. 제출 전에는 invalid handle이다.
    const WorkerTaskHandle& Handle() const noexcept;

    // terminal 여부는 Handle().Status()로 판정한다. terminal 전 Busy, 제출한 적 없음 StaleHandle.
    // 실행하지 않은 작업(취소·Shutdown(Cancel)·의존 실패)은 상태를 그대로 옮긴다(Cancelled·DependencyFailed).
    // 실행한 작업은 결과 slot을 우선한다. 성공했는데 늦게 취소가 걸려 handle 상태가 Cancelled인 경우에도 결과는
    // 성공(데이터 유효)이며, 이 handle에 의존한 후속 작업은 DependencyFailed가 된다.
    Result<usize> TryGetResult() const noexcept;
    Result<usize> TryWaitResult(uint32 timeoutMs = platform::InfiniteTimeout) const noexcept;
};

Result<void> TrySubmitCompress(WorkerScheduler& scheduler, CompressionTask& task, const CompressionSettings& settings,
                               Span<const uint8> source, Array<uint8>& output,
                               Span<const WorkerTaskHandle> dependencies = {}) noexcept;
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task, CompressionFormat format,
                                 Span<const uint8> source, usize maxOutputBytes, Array<uint8>& output,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;
Result<void> TrySubmitDecompress(WorkerScheduler& scheduler, CompressionTask& task, CompressionFormat format,
                                 Span<const uint8> source, Span<uint8> destination, Allocator& scratch,
                                 Span<const WorkerTaskHandle> dependencies = {}) noexcept;

} // namespace rocks
```

- **할당기**: body `Function`은 Append 변형에서는 `output.GetAllocator()`, Span 변형에서는 scratch에서 할당한다. 이
  할당기는 output 성장·코덱 scratch·capture 해제를 worker에서 수행하므로 소유 스레드와 **동시에 써도 안전해야** 한다.
  System·Binned·Budget·Pool·External·Debug는 허용하고 Arena·Stack은 금지한다. scheduler 할당기는 task보다 오래 살아야 한다.
- **제출 순서**: 제출은 다음 순서로 진행한다.
  1. output을 읽지 않는 동기 검증(Lz4 입력 한계·빈 해제 입력·모르는 형식·Span 변형의 source·destination 겹침)과 사용자
     등록 형식의 사용권 획득(해제됐거나 해제 중이면 InvalidArgument).
     output은 앞선 의존 작업이 worker에서 키우는 중일 수 있어 제출 시점에 읽지 않는다(읽으면 데이터 경합). output과의
     겹침·크기 overflow는 body가 실행 시점에 검사한다.
  2. 진행 중 task면 Busy
  3. `Function::TryCreate`(실패 시 아무것도 바뀌지 않음)
  4. 이전 결과 보관 후 slot 초기화(worker가 Submit 반환 전에 실행할 수 있음)
  5. Submit
     - 실패하면 slot을 복원하고, 보존된 Function은 호출 스레드에서 파괴한 뒤 scheduler 오류(ShuttingDown·
       CapacityExceeded·WrongContext·InvalidArgument·OutOfMemory)를 그대로 반환한다.
     - 성공하면 handle을 보관한다.
- **본문**
  - output의 원래 길이는 body 시작 시점에 기록한다. 의존으로 직렬화한 여러 작업이 같은 output에 이어 붙일 수 있다.
  - source의 주소·길이는 제출 시점에 고정된다. 앞선 작업은 내용만 채울 수 있다.
  - 코덱 오류는 `context.Fail(error)`로 보고한다. 보고하지 않으면 Succeeded가 되어 후속 작업이 잘못된 데이터로 실행된다.
    코덱이 Cancelled를 돌려줘도 실제 취소 요청이 없으면 실패로 보고한다.
- **취소**
  - 협력적이며 body 시작 전과 분할 단위(1MiB)마다 확인한다. zlib 계열과 zstd 압축은 입력 소비나 출력 생성 1MiB마다,
    zstd 해제는 stable output 방식이라 입력 1MiB마다 확인한다(압축률이 아주 높은 입력은 한 번의 호출로 큰 출력을 만든다).
  - `IsCancellationRequested`는 scheduler gate를 잡으므로 더 촘촘히 확인하지 않는다.
  - 취소를 관측하면 output 길이를 복원하고 Fail 없이 반환해 Cancelled가 된다.
  - 취소 가능 경로: zlib 계열 전체, zstd 압축·해제, LZ4 frame 압축(입력 1MiB마다)·해제(입력 소비나 출력 생성 1MiB마다),
    Brotli 해제(입력 소비나 출력 생성 1MiB마다).
  - 끊지 않는 경로는 LZ4 raw block 압축·해제다. LZ4 fast·해제는 수 초 이내지만 HC 12는 약 10–20MB/s여서 2GiB 가까운 입력이면
    100초 이상 걸린다. `Shutdown`은 worker를 Join하므로 Cancel 모드여도 그만큼 기다린다. 큰 입력은 블록 컨테이너(후속)를
    쓴다.
- **소멸**: 진행 중인 task의 소멸자는 취소를 요청하고 기다린다. 대기 중인 작업의 취소는 worker가 처리하므로 모든 worker가
  막혀 있으면 소멸자도 기다린다. 같은 scheduler의 worker(다른 작업의 body·capture 정리 포함)에서는 기다릴 수 없어 Fatal이므로
  task를 다른 작업의 capture가 소유하게 하지 않는다. 다른 스레드는 `Handle().Clone()`으로 얻은 handle로만 기다린다.
- **파일 읽기와 연결(AsyncFile 완료 → 해제)**: 압축된 entry를 비동기로 읽고 읽기가 끝나면 해제가 실행되게 한다.
  `WorkerScheduler::TryCreateEvent`로 외부 완료 이벤트(`WorkerTaskEvent`)를 만들고, 그 handle에 의존하는 해제를 먼저
  제출한 뒤, `TrySubmitReadEvent(file, request, offset, buffer, bytes, Move(event), allocator)`(`<Task/AsyncIoEvent.h>`)로
  읽기를 시작한다. 읽기가 요청 byte를 모두 채우면 이벤트가 Trigger되어 해제가 실행되고, 짧은 읽기·IO 오류면 이벤트가 Fail해
  해제는 실행되지 않고 DependencyFailed다([WorkerScheduler](../Implementation/WorkerScheduler.ko.md)).

  ```cpp
  auto event = scheduler.TryCreateEvent();                      // Result<WorkerTaskEvent>
  auto after = Span<const WorkerTaskHandle>(&event.Value().Handle(), 1);
  TrySubmitDecompress(scheduler, task, CompressionFormat::Brotli,
                      Span<const uint8>(buffer.Data(), buffer.Size()), rawBytes, output, after);
  TrySubmitReadEvent(file, request, offset, buffer.Data(), buffer.Size(), Move(event.Value()), allocator);
  auto bytes = task.TryWaitResult();                           // 읽기 완료 → 해제
  ```

  - source의 주소·길이는 해제 제출 시점에 고정되고 IO는 내용만 채운다. 그래서 buffer는 제출 전에 entry 크기로 확보하고
    (해제가 먼저 제출되어도 이벤트 완료 전에는 실행되지 않는다) 읽기 완료까지 크기를 바꾸지 않는다.
  - 해제는 실행되면 1MiB(`CompressionChunkBytes`) 조각마다 취소를 확인한다. Brotli·LZ4 frame·zlib 계열은 입력 소비나 출력
    생성, zstd는 입력 소비 기준이고, Lz4 raw block은 한 번의 코덱 호출이라 확인하지 않는다(위 취소 항목). 큰 entry도 읽기 한
    번, 해제 작업 하나로 처리하며 취소는 조각 단위로 반영된다.
  - 이벤트의 완료 권한은 읽기 요청이 가져가고, 완료 문맥은 allocator(IO 스레드 해제 허용)에 만들어 콜백이 마지막에
    해제한다. 이벤트 상태를 따로 보려면 넘기기 전에 `Handle().Clone()`을 받아 둔다.
  - 시험: `Compression_AsyncDecompressAfterAsyncFileRead`(실제 파일 읽기 → zstd 해제 40회, 짧은 읽기에서 해제 미실행).
    형식과 무관한 경로라 Brotli도 같다.
- **용량**: scheduler의 `maxInFlight`(기본 256)를 넘으면 CapacityExceeded다. caller가 제출 속도를 조절한다. task는 이동할
  수 없어 `Array` 원소가 될 수 없으므로 C 배열이나 `UniquePtr`로 둔다.
- **사용자 등록 형식의 사용권**: 제출할 때 얻은 사용권을 body capture가 가진다(capture는 이동 전용이며 `[job = Move(job)]`).
  실행하면 일회성 경로로 넘겨 실행이 끝날 때, 실행하지 않으면(취소·의존 실패·Shutdown) capture가 정리될 때 놓는다. 둘 다
  terminal 게시 전이므로 `TryWaitResult`가 결과(완료·실패·취소)를 돌려준 직후의 `TryUnregisterCompressionCodec`은
  성공한다(시간 초과 Busy·WrongThread로 돌아온 경우는 아니다). body 할당·제출이 실패하면 그 자리에서 놓는다. 대기 중에
  해제가 시작돼도 작업은 끝까지 실행된다(새 제출만 막힌다, 14.3절).

## 6. Godot·UE 대응

| Godot `Compression` | UE `FCompression` | Rocks |
|---|---|---|
| `compress(dst, src, size, mode)` | `CompressMemory` | `TryCompress` / `TryCompressAppend` |
| `get_max_compressed_buffer_size` | `CompressMemoryBound` | `TryGetCompressBound` |
| `decompress(dst, dst_max, src, size, mode)` | `UncompressMemory` | `TryDecompress` |
| `decompress_dynamic(vec, max, …)` | — | `TryDecompressAppend`(상한 필수) |
| 모드 FASTLZ·DEFLATE(실제는 zlib 래퍼)·ZSTD·GZIP·BROTLI(해제) | NAME_Zlib·Gzip·LZ4·Oodle | Lz4(raw block)·Lz4Frame·Deflate(raw)·Zlib·Zstd·Gzip·Brotli(해제 전용, 13절) |
| 전역 static 수준 설정 | flags(BiasSpeed·BiasSize) | 호출마다 `CompressionSettings` |
| thread_local zstd 문맥 | — | 명시 `Compressor`·`Decompressor` |
| — | IoStore 내부 비동기 해제 | `TrySubmitCompress`·`TrySubmitDecompress` |
| `FileAccessCompressed`(블록·seek) | `FCompressedBuffer` | `CompressedBlocksWriter`·`CompressedBlocksReader`(9절) |
| `StreamPeerGZIP` | — | `CompressStream`·`DecompressStream`(10절, gzip·zlib·deflate·zstd·LZ4 frame, Brotli는 해제만) |
| — | — (`FCompression`·`FCompressedBuffer`에 사전 인자 없음. 학습 사전은 OodleNetwork 플러그인의 패킷 압축이 쓰고 commandlet이 학습) | `CompressDictionary`·`DecompressDictionary`(11절) |
| — | `ICompressionFormat`(`IModularFeatures` "CompressionFormat", `FCompression::GetCompressionFormat(FName)`, `GetVersion`·`GetDDCKeySuffix`) | `CompressionCodec`·`TryRegisterCompressionCodec`·`TryFindCompressionFormat`·`TryGetCompressionFormatInfo`(14절) |

## 7. 검증 계획

- **독립 golden**
  - 형식 명세로 직접 만든 벡터: LZ4 literal-only·match 포함 block, RFC 1951 stored·고정 Huffman block, RFC 8878 raw·RLE
    block frame, skippable frame
  - 다른 구현이 만든 gzip: GNU gzip 1.14, 이름 포함·연속 member
  - 알려진 zlib 벡터(`"abc"` → `78 9C 4B 4C 4A 06 00 02 4D 01 27`)
- **왕복**: 형식 × 수준(최소·기본·최대, LZ4 10–12 포함) × 데이터. 데이터는 빈 입력, 1byte, 0으로 채운 1MiB, 고정 seed 난수
  256KiB, 텍스트형, 64KiB±1·128KiB+1 경계를 쓴다. bound ≥ 실제 크기도 확인한다.
- **경계**
  - destination 1byte 부족, `maxOutputBytes` 정확히 같음(성공)과 1 작음(실패)
  - zlib avail 둘 다 0, zstd 거짓 크기(작게·크게), skippable만 있는 입력(성공 0), frame 뒤 1–4byte와 5byte 이상
  - FDICT, dictID frame, gzip 헤더 플래그 조합, 1MiB 분할 루프(1MiB를 넘는 입력)
- **손상**: 고정 seed bit flip, 모든 길이로 자른 입력, 무작위 입력을 넣는다. 오류이거나, 성공하면 원본과 같아야 한다(무결성
  없는 형식은 범위 밖 쓰기가 없음만 확인). ASan 구성에서 실행한다.
- **폭탄 입력**: 2^40을 선언한 zstd frame, 1KB → 1GB zlib을 상한 1MiB로 해제한다. 계수 allocator의 최고 사용량이
  상한 + 고정 scratch 이하여야 한다.
- **할당**
  - NFailAllocator로 모든 할당 시도 위치에서 실패시킨다. OutOfMemory, 누수 0, output 복원을 확인한다.
  - 계수 allocator로 zstd·zlib 문맥이 주입 할당기를 쓰는지, LZ4 HC 압축의 할당이 0인지 확인한다.
- **재사용**: 실패(CapacityExceeded·OOM·손상) 직후 같은 문맥으로 다시 호출한 결과가 새 문맥·자유 함수의 결과와 byte 단위로
  같아야 한다(gzip OS byte 포함).
- **결정성**: 압축 결과 CRC-32 golden, 동기·비동기 출력 동일성.
- **비동기**
  - 8 worker × 64건 왕복과 8 producer 동시 제출(스레드 안전 할당기 하나)
  - 취소: pending 상태 취소(blocker 작업 사용), 실행 중 취소(할당 hook으로 멈춤), 늦은 취소(capture 해제 hook으로 멈춤)
  - Shutdown Drain·Cancel, DependencyFailed에서 body 미실행, 진행 중 재제출 Busy
  - OOM: Function·scheduler record·worker 안 코덱 할당 실패(원자적 계수 NFail 할당기 필요)
  - ASan 100회 반복
- **빌드·정책**
  - C++ 인자 golden, C 모듈 실제 빌드, include된 `.c` 변경 시 무효화
  - 3.5의 음성 대조, 진단 지시문을 제거한 원본 사본의 경고 0 자동 검사
- **플랫폼**: Windows x64 로컬. macOS ARM64·Android는 CI·실기기 결과가 생기기 전까지 미검증으로 표시한다. LZ4 1.11의
  Android 빠른 해제 루프는 과거 칩셋 회귀 사례가 있어 실기기로 측정한다.

## 8. 구현 상태와 검증 (2026-11-12)

| 항목 | 결과 |
|---|---|
| 원본 무결성 | zstd 공식 `.sha256` 일치, zlib `.asc` GPG 서명 정상(지문 5ED4…AFBA), LZ4·zlib GitHub 자산 digest 일치. 저장소 원본 81개 파일의 SHA-256이 manifest와 일치 |
| 빌드 | clang 21(clang-cl)로 Debug·ASan·Release(ThinLTO, C bitcode 포함) Foundation 타깃 경고 0 |
| Compression 시험 | 17개(동기 10·비동기 7: 실행 중·늦은 취소, 같은 output 연쇄 Append 포함)가 Debug·ASan·Release에서 통과. Foundation 전체 통과 |
| 빌드 도구 시험 | `Rocks.Build.Tests` 21개 그룹 통과(C++ compile action golden Debug·Release, C17 프로필, 실제 C 빌드·실행 Debug·Release, include된 `.c` 무효화, 언어 변경 인자 거부) |
| 정책 시험 | `Rocks.ToolTests self` 72개 통과(third-party 저장소 검사와 음성 대조). `native` 96개 통과(실제 compile DB 언어·경고 감사, 지시문 무효 확인과 그 양성·음성 대조) |
| 다른 플랫폼 | GitHub Actions(macOS 15 ARM64, Apple clang)에서 01f1b98·64ff35f의 Compression 시험이 모두 통과(64ff35f: 17개, 1.5MiB 결정성 golden 포함). Android NDK arm64·x86_64 교차 빌드 경고 0(C 번역 단위 31개가 `clang++ -x c -std=c17`), Android 35 x86_64 에뮬레이터에서 17개 통과(golden 포함). 세 플랫폼에서 압축 결과 byte가 같음을 확인 |
| 2차(스트리밍·블록 컨테이너) | 스트림 14개·블록 컨테이너 16개 시험(결정성 golden 각 1개 포함)이 Windows x64 Debug·ASan에서 통과. 구현 뒤 독립 검토 5건(zstd·zlib adapter, 스트림 드라이버, RCBK 형식·reader, 병렬·writer)의 확정 결함을 모두 반영했다: zlib 작은 창 flush 무한 반복(major), 읽기 문맥 캐시의 reader 주소 재사용 혼동(major), 병렬 경로가 worker가 빌 때까지 기다리다 교착(major, `WorkerTaskHandle::TryRevoke` 추가), 그 밖의 오류 분류·캐시 검증·helper 자원 실패 처리. macOS·Android 실행은 아직 하지 않았다 |
| 공용 변경 | `Array::TryAppendUninitialized`(0 채우기 없는 성장), trivial 소멸 타입 축소·`Clear`를 O(1)로(Debug 스트림 시험 9.5초 → 1.1초). 1차 Append 경로도 0 채우기를 없앴다 |
| 3차(zstd 사전, 11절) | 설계 교차검증 3건(zstd 원본·실측, API·계약·형식, dictBuilder 조사) 뒤 구현. 사전 시험(11.7절)이 Windows x64 Debug·ASan에서 통과. 구현 뒤 독립 검토 2건(수명·동시성·오류 분류, zstd 의미·결정성·형식·시험 공백)의 확정 항목을 반영했다: 이동된 블록 설정이 사전 없이 조용히 다른 결과를 냄, 큰 window frame의 사전 ID 조회 분류, 스트림의 사전 ID·window 검사 순서, 지문이 같고 내용이 다른 후보, 헤더 배치 변경의 version 미갱신(RCBK v2), 문서 오류. 검토가 만든 시험 18개를 저장소로 옮겼고, 사전과 무관한 기존 결함(일회성 zstd 해제가 window 상한 초과 frame을 InvalidArgument로 분류)도 Unsupported로 고쳤다. Foundation 전체 908개 통과(Debug). 표준 경로 등가성은 원본 내부 함수로 1,367만 건, 실측 8,328건을 비교해 반례가 없었다. macOS·Android 실행은 아직 하지 않았다 |
| 4차(사전 학습, 11.6절) | 설계 교차검증(dictBuilder 원본·실측: 예외 경로 충돌 재현, 기본값 시간·이득 비교) 뒤 구현. 학습 시험 8개와 manifest `licenses` 이전 시험(Policy.ThirdPartyLicenses·ThirdPartySdkLicenses)을 포함해, HEAD 기반 worktree에서 Foundation Debug·ASan 916개, Release 압축 82개, `self` 84개, `native` 95개, 빌드 도구 시험 22개 그룹, 정책·형식 검사가 통과했다. 구현 뒤 독립 검토의 확정 항목(표본 수 상한 1 초과, POSIX 종료 중 mutex 소멸, finalize 헤더 버퍼 초과 분류, 빈 표본 포인터)을 반영했다. 학습 golden은 upstream 원본만으로 빌드한 probe로 Windows x64·Android x86_64에서 같은 byte임을 확인했다. arm64는 아직 확인하지 않았다 |
| 5차(LZ4 frame, 12절) | 공식 tarball(sha256 일치)의 `lz4frame.[ch]`·`xxhash.[ch]`를 들이고 upstream 원본만으로 빌드한 probe로 설정·호출 순서·할당 경로를 먼저 확인한 뒤 구현했다. 시험 24개(12.6절)를 포함해 HEAD 기반 worktree에서 Foundation Debug·ASan·Release 949개, `self` 84개, `native` 95개(Debug compile DB: 지시문을 지운 `lz4frame.c`·`xxhash.c` 사본 경고 0 포함), 정책·형식·third-party 검사가 통과했다. 일회성 결과는 upstream `LZ4F_compressFrame`과 byte 단위로 같다(수준 1·2·9·12, 1.5MiB). 구현 뒤 독립 검토 2건(adapter 의미, 시험·문서·manifest)의 확정 항목을 반영했다: 손상된 FLG의 사전 ID bit가 Unsupported로 분류됨(헤더를 LZ4F로 먼저 검증하도록 수정), legacy frame 분류, 기본 할당 경로를 저장소 안에서 막는 `LZ4_USER_MEMORY_FUNCTIONS`, 스트림 경계·상한·수준 2·12·Append 성장 OOM·출력 기준 취소 시험 보강, 문서 수치. Android NDK arm64 문법 검사(지시문 유무 모두)는 경고 0이고, macOS·Android 실행은 아직 하지 않았다 |
| 6차(Brotli 해제, 13절) | google/brotli 1.1.1 tag archive(sha256, tag tree blob hash 비교)의 해제기만 들였다. 시험 15개(13.7절)를 포함해 HEAD 기반 worktree에서 Foundation Debug·ASan·Release 969개, `self` 85개, `native` 95개(Debug compile DB), 정책·형식·third-party 검사가 통과했다. 참조 벡터는 google/brotli 1.1.0 encoder(Python binding)가 만들었다. llvm-nm으로 CRT 할당 참조가 기본 할당 경로 오브젝트 3개에만 있고 심볼 충돌이 없음을 확인했다. Android NDK arm64·x86_64 문법 검사(Brotli C17 `-Wall -Wextra -Wpedantic -Werror` `-O0`·`-O2`, adapter C++23)는 경고 0이다. 구현 뒤 독립 검토 2건(adapter 의미, 시험·문서·manifest)의 확정 항목을 반영했다: ring buffer를 늘리는 순간의 최고 사용량(약 1.5 × window) 문서·시험, large window 표지 판정(0x91은 손상)과 Append의 거부 전 출력 확보, 사전 오류 분류, 공개 헤더 WOFF2 예제의 해제 크기(totalSfntSize가 아니라 표 길이 합), CRT 경로 목록, 4·10절의 Brotli 누락, 벤치 측정 구간, 뒤 byte 조각·Reset OOM 시험 보강. macOS·Android 실행은 아직 하지 않았다 |
| 7차(형식 플러그인, 14절) | 설계 교차검증(값 인코딩·수명·비동기 연동·거부 표면) 뒤 구현했다. 시험 13개(14.5절)를 포함해 HEAD 기반 worktree에서 Foundation Debug·ASan 989개, Release 압축 83개, 플러그인 시험 ASan 30회 반복, `self` 85개, `native` 95개, 정책·형식 검사가 통과했다. 동시성 시험은 해제의 Busy 검사를 끈 변이를 사용권 불변식 위반으로 잡는다. 구현 뒤 독립 검토 2건(수명·동시성·오류 분류, 시험·문서)의 확정 항목을 반영했다: 비동기 사용권을 놓는 시점 문서, 첫 크기가 상한보다 작아 여러 번 다시 해제하던 문제(Lz4 raw block 포함), 두 배 재시도의 OutOfMemory 문서, 해제된 형식 설정으로 만든 압축 사전의 오류 분류, 동시성 시험의 검출력(heap 코덱 파괴·겹침 확인), 실행 중 Busy·재시도 사이 취소·실행되지 않은 비동기 작업·해제 중 칸 용량 시험, 시험 사이 등록 누수 방지(`ScopedCodec`), clang-format이 접은 주석. macOS·Android 실행은 아직 하지 않았다 |

처리량(로컬 1회 측정, 1차 때 저장소 밖 임시 벤치): Windows x64 Release(ThinLTO), 단일 스레드, `Compressor`·`Decompressor`
재사용, 저장소 소스·문서 8.9MiB, 여러 번 중 최고값이다. 기기 사양을 고정한 반복 측정이 아니므로 기준값이 아니라 adapter가 원본
라이브러리 성능을 해치지 않는다는 확인으로 본다. 지금은 저장소의 `Programs/CompressionBench`(고정 seed 합성 코퍼스 또는
`--file`, 일회성·스트림·블록 컨테이너 순차/병렬)로 같은 측정을 반복할 수 있다.

| 형식 | 수준 | 압축 비율 | 압축 MB/s | 해제 MB/s |
|---|---|---|---|---|
| Lz4 | 1 / 9 / 12 | 0.401 / 0.293 / 0.290 | 719 / 44 / 19 | 4770 / 4664 / 5018 |
| Zstd | 1 / 3 / 9 / 19 | 0.286 / 0.248 / 0.214 / 0.188 | 496 / 367 / 92 / 4.0 | 1473 / 1440 / 1576 / 1520 |
| Deflate·Zlib·Gzip | 1 / 6 / 9 | 0.314 / 0.256 / 0.254 | 171 / 46 / 17 | 480 / 483 / 517 |

`CompressionBench --size 32 --workers 8`(합성 32MiB, Release, 같은 기기 1회): 블록 컨테이너(256KiB 블록) 처리량 MB/s.

| 형식 | 수준 | 순차 압축 | 병렬 압축(8) | 순차 읽기 | 병렬 읽기(8) |
|---|---|---|---|---|---|
| Lz4 | 1 | 1052 | 2950 | 2824 | 7711 |
| Zstd | 3 | 381 | 1830 | 1070 | 4435 |
| Deflate | 6 | 20 | 128 | 435 | 2382 |

스트림(64KiB 조각): zstd 3은 압축 267 / 해제 999MB/s, gzip 6은 19 / 416MB/s(같은 코퍼스의 일회성 342 / 1151, 19 / 465).

사전(`CompressionBench --size 16`, Release, 같은 기기 1회): 합성 코퍼스 앞 4MiB를 1KiB·4KiB 항목으로 나눠 항목마다 zstd
frame 하나로 압축·해제한다. 사전은 학습하지 않은 raw 내용(항목 구간 바로 뒤 64KiB)이라 학습 사전보다 효과가 작은 하한이다.

| 항목 | 수준 | 압축률(사전 없음 → 있음) | 압축 MB/s(없음 → 있음) | 해제 MB/s(없음 → 있음) |
|---|---|---|---|---|
| 1KiB | 3 | 0.475 → 0.335 | 167 → 247 | 420 → 543 |
| 1KiB | 19 | 0.457 → 0.335 | 11.0 → 5.7 | 409 → 391 |
| 4KiB | 3 | 0.376 → 0.303 | 230 → 257 | 675 → 559 |
| 4KiB | 19 | 0.344 → 0.294 | 8.2 → 5.5 | 693 → 668 |

작은 항목일수록 이득이 크다. 수준 19는 사전까지 탐색해 압축이 느려진다.

미검증·후속:
- Android arm64 실기기 실행은 미검증이다(arm64는 교차 빌드만 확인).
- 기기 사양을 고정한 반복 측정 기록, `DYNAMIC_BMI2=0`·LZ4 1.11 Android 해제 루프의 영향 측정은 후속이다.
- 루트를 include하는 시험 모듈은 증분 키를 계산할 때 ThirdParty `.c`(약 1.6MB)도 해시한다. 빌드 실행 안에서 파일 hash·
  디렉터리 목록을 재사용하는 memoize를 구현해 재 보았으나(2026-11-12) Foundation Debug no-op 빌드가 전후 모두 13–16초로
  차이가 없었다. 저장소 header·`.c`는 270개뿐이고, 루트 include가 없는 작은 타깃(CompressionBench)도 no-op에 6.6초가 걸려
  고정 비용이 대부분이다. 효과 없는 복잡도라 넣지 않았고, no-op 빌드 시간은 단계별 계측으로 병목을 찾은 뒤 다룬다.

## 9. 블록 컨테이너 `CompressedBlocks` (2차, 교차검증 반영)

큰 데이터를 고정 크기 블록으로 나눠 각각 압축하고, 임의 위치를 필요한 블록만 해제해 읽는다(Godot
`FileAccessCompressed`, UE `FCompressedBuffer`·`FCompressedBufferReader` 대응). 블록이 독립이라 병렬 압축·해제가 되고,
블록마다 저장 byte의 CRC로 무결성을 확인한다(LZ4·Deflate raw처럼 체크섬이 없는 코덱도 보호). 헤더는
`Compression/CompressedBlocks.h`다.

### 9.1 영속 형식 RCBK v2

모든 정수는 little-endian이며 byte 단위로 읽는다(컨테이너가 Append로 임의 위치에 놓일 수 있어 struct 재해석을 쓰지 않는다).

| offset | 크기 | 필드 | 거부 |
|---|---|---|---|
| 0 | 4 | magic `RCBK` | InvalidArgument |
| 4 | 2 | version = 2(1은 사전 지문 없는 40byte 헤더였다) | 다르면 Unsupported |
| 6 | 1 | codec ID(영속 값): 1 Lz4, 2 Zstd, 3 Deflate | 0은 InvalidArgument, 4 이상은 Unsupported |
| 7 | 1 | blockSizeLog2 12–24(4KiB–16MiB) | InvalidArgument |
| 8 | 8 | rawSize | |
| 16 | 4 | blockCount = ⌈rawSize / blockSize⌉ | 다르면 InvalidArgument |
| 20 | 4 | flags: bit0 = 사전(11.4절) | 모르는 bit, bit0인데 codec ≠ Zstd는 Unsupported |
| 24 | 8 | payloadSize = 저장 크기 합 | |
| 32 | 8 | dictionaryFingerprint = 사전 SHA-256 앞 8byte(bit0 = 0이면 0) | bit0 = 0인데 0이 아니면 InvalidArgument |
| 40 | 4 | reserved = 0 | 0이 아니면 InvalidArgument |
| 44 | 4 | indexCrc32 = CRC-32(header[0, 44) ‖ block table) | InvalidArgument |

- block table: blockCount × {`storedSize: u32`, `storedCrc32: u32`(그 블록 저장 byte의 CRC-32)}. 그 뒤 payload는 블록 저장
  byte를 순서대로 이어 붙인다.
- raw 저장 ⇔ storedSize == 블록 원본 크기, 압축 ⇔ 1 ≤ storedSize < 블록 원본 크기. storedSize만 보고 raw·압축이 하나로
  정해진다(같은 원본의 저장 byte가 하나뿐이라는 뜻은 아니다: 압축 가능한 블록을 raw로 둔 컨테이너도 유효하다).
  마지막 블록 원본 크기 = rawSize − (n − 1)·blockSize.
- 헤더만으로도 크기 일관성을 검사한다: n ≤ payloadSize ≤ rawSize, 48 + 8n + payloadSize == 컨테이너 길이(overflow 없이
  계산). 따라서 n ≤ 컨테이너 길이 / 9이고, 48byte짜리 악성 헤더로 큰 인덱스 할당을 유도할 수 없다.
- 블록 코덱 데이터(writer 규칙): Lz4 raw block, Zstd frame 하나(content size 포함, checksum 끔 — 컨테이너 CRC가 대신한다),
  Deflate raw. reader는 해제 결과가 정확히 블록 원본 크기인지만 강제한다(zstd frame 여러 개·checksum 포함 frame도 결과가
  맞으면 받는다). 사전 컨테이너의 zstd frame은 사전 ID를 기록한다. 사전 없는 컨테이너의 사전 블록과 사전 컨테이너의 ID
  없는·다른 ID 블록은 형식 위반이라 InvalidArgument다.
  Zlib·Gzip은 래퍼 중복이라 허용하지 않는다. 블록 압축 결과가 원본 이상이면(용량 = 원본 − 1에서 CapacityExceeded) raw로
  저장한다. Deflate 수준 0은 항상 raw다.
- CRC-32는 zlib `crc32_z`(Core `Crc32`와 같은 값)로 저장 byte에 대해 계산한다. 원본이 아니라 저장 byte라 압축률만큼 싸고,
  해제 전에 손상을 거르며, 해제 없이 무결성만 검사할 수 있다. 읽을 때 검증을 끌 수 있다(기본 켬). CRC는 우발 손상만
  잡는다. CRC를 다시 맞춘 의도적 변조는 거르지 못하므로(LZ4·Deflate는 다른 데이터로 성공할 수 있다) 신뢰할 수 없는 출처의
  무결성은 서명·해시로 따로 확인한다.
- 헤더 검사 순서: magic(InvalidArgument) → version → codec ID → flags(모르는 bit, 사전 bit + Zstd 아닌 codec. 여기까지
  Unsupported) → blockSizeLog2·reserved·사전 bit 없는 지문·블록 수·payload·전체 길이(InvalidArgument) → (TryOpen)
  indexCrc32 → 사전 후보 선택(Unsupported) → 저장 크기 규칙·합계. 그래서 version·flags가 손상돼도 Unsupported로
  보고되고, 지문 byte 손상은 CRC에서 InvalidArgument로 걸려 "사전 불일치"로 잘못 보고되지 않는다.
- 원본 크기 증폭: 블록 하나가 1byte로 저장될 수 있어 rawSize는 컨테이너 길이의 최대 약 186만 배다(57byte → 16MiB). reader는
  이만큼 할당하지 않지만, `RawSize()`로 출력을 잡는 호출자는 자기 상한과 비교해야 한다.
- 결정성: 같은 라이브러리 버전·설정·입력이면 같은 byte다. 병렬 압축도 순차와 같은 byte다.

### 9.2 API

- `CompressedBlocksSettings::TryMake(compressionSettings, blockSizeLog2 = 18)`: 형식은 Lz4·Zstd·Deflate만이며 수준은
  `CompressionSettings`가 정한다. 사용자 등록 형식은 InvalidArgument다(codec ID가 파일에 남는 영속 값이라 실행마다 바뀌는
  형식 값을 기록할 수 없다).
- `TryGetCompressedBlocksBound(blockSizeLog2, rawSize)` = 48 + 8n + rawSize(블록은 원본보다 커지지 않는다).
- `TryCompressBlocksAppend(settings, source, output, scratch)`: 순차. 실패 시 output 길이 복원.
- `TryCompressBlocksAppend(scheduler, settings, source, output, scratch)`: 병렬.
  - 연속 블록 범위 job을 atomic 카운터로 worker와 호출 스레드가 함께 가져간다. job마다 자기 Array에 압축한다.
  - 끝나면 output을 정확히 한 번 늘려(0 채우기 없이) 인덱스와 job 결과를 순서대로 복사한다. 그래서 Submit 거부나
    Shutdown(Cancel)로 body가 돌지 않아도 호출 스레드가 남은 job을 처리해 결과가 완성된다.
  - 실패하면 공유 flag로 일찍 멈추고, 가장 작은 블록 index의 오류를 돌려준다(결정적: job은 번호 순서로만 나가고 가져간
    job은 끝까지 처리하므로, 실패한 job보다 앞선 job은 모두 처리된다).
  - 반환 시점: 호출 스레드가 job을 다 처리하면 아직 시작하지 않은 helper는 `WorkerTaskHandle::TryRevoke()`로 회수하고,
    실행 중인 helper가 가져간 job(16MiB 이하)을 마칠 때까지만 기다린다. 처음 구현은 모든 helper가 terminal이 될 때까지
    기다려, worker가 다른 일로 바쁘면 그만큼 늦고 호출 스레드가 쥔 lock을 worker가 기다리면 교착했다(교차검증에서 재현).
    Pending 작업을 호출 스레드에서 Cancelled로 끝내는 회수 API를 scheduler에 추가해 고쳤다.
  - helper는 job을 가져가기 전에 코덱 문맥을 만들고, 만들지 못하면 참여하지 않는다(helper 자원 부족이 호출 전체를
    실패시키지 않는다). job 결과는 job 원본 크기로 한 번 확보한다.
  - 호출 스레드가 그 scheduler의 worker면 WrongThread다. 블록 4개 이하이거나 1MiB 미만이면 순차로 처리한다. scratch는
    여러 스레드가 동시에 할당하고 다른 스레드가 해제해도 되는 할당기여야 한다(capture는 호출 스레드가 할당하고 worker가
    해제한다).
  - job 크기: worker 수 × 4개 정도의 job을 만들되 job 하나는 원본 16MiB 이하다. helper 작업은 worker 수(최대 64, 원본
    512KiB당 하나 이하)만큼 제출하고, 취소 요청을 받은 helper는 새 job을 가져가지 않는다(남은 job은 호출 스레드가 처리한다).
  - 메모리: 참여 스레드마다 코덱 문맥 하나다. zstd 고수준·큰 블록은 문맥이 크므로(수준 19·16MiB 블록 약 81MiB) worker
    수로 조절한다. job 결과는 모든 job이 끝날 때까지 보관하므로 압축 결과 크기만큼 더 쓴다.
- `CompressedBlocksWriter`(증분 writer, IO 비의존): `TryBegin(settings, rawSize)`로 인덱스 크기(48 + 8n)를 정한다. 코덱
  문맥은 처음 압축하는 블록에서 만든다(빈 원본·Deflate 수준 0은 만들지 않는다). 블록 추가가 실패하면 상태가 그대로라 같은
  블록을 다시 줄 수 있다.
  `TryAppendBlock(rawBlock, payloadOutput)`을 블록 순서대로 부르고(마지막 외에는 정확히 blockSize), `TryFinishIndex(indexOut)`로
  인덱스를 쓴다. 파일이면 인덱스 자리를 비워 두고 payload를 흘려 쓴 뒤 되돌아가 인덱스를 쓴다.
- `CompressedBlocksReader`(불변, 여러 스레드가 const로 공유)
  - `TryGetIndexBytes(header, containerBytes)`: 헤더 48byte만 검사해 인덱스 크기를 돌려준다(파일은 이만큼만 먼저 읽는다).
  - `TryOpen(index, containerBytes, allocator)`, `TryOpen(container, allocator)`: 표 CRC·저장 크기 규칙·합계를 검사하고
    블록별 저장 offset 표(16byte/블록)를 만든다.
  - `TryLocate(rawOffset, rawBytes)`: 걸치는 블록과 그 저장 byte 범위(컨테이너 기준)를 돌려준다.
  - `TryRead(rawOffset, destination, stored, storedBase, context, verify = true)`: stored는 컨테이너의 [storedBase, +크기)
    byte다. 필요한 범위를 내부에서 다시 계산해 stored 안에 있는지 확인한다(호출자가 준 범위를 믿지 않는다). 메모리 전체
    컨테이너 편의 overload는 storedBase 0이다. rawSize를 넘는 읽기는 InvalidArgument, 0byte 읽기는 빈 성공이다.
  - `TryRead(scheduler, …)`: 큰 범위 병렬 해제(블록 범위 job, 블록은 destination에 바로 해제). 걸치는 끝 블록만 job별
    임시 버퍼를 쓴다.
  - `TryVerify(stored, storedBase)`: 해제 없이 모든 블록 저장 byte의 CRC를 확인한다.
- `CompressedBlocksReadContext`: 스레드마다 하나. 같은 형식의 Decompressor, 블록 크기 버퍼 하나, 마지막으로 해제한
  블록 캐시를 가진다. 블록 일부만 걸치는 읽기는 이 버퍼로 해제하고, 같은 블록을 이어 읽으면 다시 해제하지 않는다.
  - `TryCreate(reader, allocator)` 또는 `TryCreate(format, blockSizeLog2, allocator)`. 형식이 같고 블록 크기가 같거나 큰
    문맥이면 여러 reader에 쓸 수 있다.
  - 캐시 식별은 (reader가 `TryOpen`에서 받는 64bit 고유 번호, 블록)이다. 처음에는 위치 표 주소로 식별했으나, reader를
    닫고 새 reader가 같은 주소를 받으면 다른 컨테이너의 블록이 반환되는 결함이 교차검증에서 나와 고쳤다. 같은 reader로
    읽는 저장 byte 자체를 바꾸면 `Invalidate()`를 부른다.
  - 캐시를 써도 stored가 블록을 덮는지 확인하고, verify 없이 채운 캐시를 verify 읽기가 쓰면 그때 CRC를 확인한다(결과가
    캐시 상태에 따라 달라지지 않는다).
- 오류: 형식 위반·길이·CRC 불일치·블록 해제 결과 크기 불일치는 InvalidArgument다. 블록 안 코덱의 CapacityExceeded도
  InvalidArgument로 바꾼다(블록 원본 크기는 표가 보장한다). 모르는 version·flags·codec ID는 Unsupported다.

## 10. 스트리밍 `CompressStream`·`DecompressStream` (2차, 교차검증 반영)

전체 크기를 모르거나 메모리에 다 올리지 않는 입력을 조각 단위로 압축·해제한다(Godot `StreamPeerGZIP` 대응. Godot의
deflate 스트림은 zlib 래퍼라 Rocks `Zlib`에 대응한다). 형식은 Zstd·Deflate·Zlib·Gzip·Lz4Frame이며, Lz4 raw block은 스트림
형식이 아니라 Unsupported다. Brotli는 해제 스트림만 있다(압축 스트림 Unsupported). LZ4 frame 스트림의 세부 사항은 12.4절,
Brotli는 13.4절이다. 헤더는 `Compression/CompressionStream.h`다.

- 기본 API는 역압이 있는 `TryProcess(input, output) → StreamProgress{consumed, produced, complete}`다. 출력이 모자라면
  오류가 아니라 일부만 소비하고 돌아오므로, 작은 입력 하나가 큰 출력을 한꺼번에 만들지 않는다. `TryWriteAppend`
  (입력을 모두 소비하고 Array 뒤에 붙임)는 편의 함수다.
- 압축: `TryProcess`, `TryFlush`(네트워크용 동기 flush: Z_SYNC_FLUSH·ZSTD_e_flush), `TryFinish`(complete까지 반복),
  `Reset`. 결정성은 같은 호출 순서(입력 조각·출력 용량)일 때만 성립하고 일회성 API와 byte가 같다고 약속하지 않는다.
  Deflate 수준 0은 출력 용량에 따라 stored block 경계가 바뀐다. Append 편의 함수는 고정 크기(1MiB)로 출력을 늘려 같은 입력
  조각이면 같은 byte를 만든다. zstd 압축 스트림은 크기를 모르므로 수준별 표를 최대로 쓴다(수준 19는 약 90MiB). 선택적 크기
  힌트를 받는다.
- 압축 단계: Open → (TryFlush 미완료) Flushing → Open, (TryFinish 미완료) Finishing → Finished. zlib·zstd는 미완료
  flush를 같은 모드로 다시 부르기를 요구하므로, Flushing에서는 TryFlush만, Finishing에서는 TryFinish만 받는다(그 밖은
  InvalidArgument, 상태 불변). Finished에서 TryFinish는 {0, 0, true}, Process·Flush는 InvalidArgument다.
- 출력 공간이 0인 호출은 코덱을 부르지 않고 {0, 0, false}다(역압과 같은 의미).
- 해제: `DecompressStreamLimits{maxOutputBytes(필수, 0이면 InvalidArgument), maxWindowLog = 27(10–31, zlib 계열과
  LZ4 frame은 범위만 검사, Brotli는 stream 첫 byte의 WBITS와 비교)}`.
  - 출력 누계(member·frame 합)가 상한을 넘으면 CapacityExceeded다(정확히 상한이면 성공). `TryProcess`는 코덱에 주는
    출력 창을 남은 허용량 + 1로 줄여 1byte 초과로 감지한다. `TryWriteAppend`는 output을 남은 허용량까지만 늘리고, 허용량을
    다 쓰면 1byte 임시 창으로 블록 끝·체크섬까지만 진행한다. 그래서 output이 상한보다 길어지지 않는다.
  - zstd는 frame이 시작될 때마다 헤더를 미리 검사한다. window(single segment frame은 원본 크기)가 2^31을 넘으면
    Unsupported(구현 상한), 2^maxWindowLog만 넘으면 CapacityExceeded(호출자 상한)다. zstd의 단일 패스 경로가 window 상한
    검사를 건너뛰므로 adapter가 먼저 막는다.
  - zstd 스트림 해제기는 내부 버퍼에 먼저 풀어 나눠 내보내므로, 이 경로의 `dstSize_tooSmall`은 출력 부족이 아니라 블록이
    frame 정보를 어긴 손상이다. InvalidArgument로 분류한다.
  - 스트림이 끝난 뒤의 byte: Gzip은 다음 member(inflate가 magic을 직접 검사), Zstd·Lz4Frame은 다음 frame, Deflate·Zlib·
    Brotli는 InvalidArgument다.
  - `IsAtStreamBoundary()`는 지금까지의 입력이 완결된 스트림(들)인지다. `TryFinish()`는 경계가 아니거나 스트림이 하나도
    없으면 InvalidArgument다(skippable frame만 있으면 성공, 출력 0). 출력은 frame·member 끝의 체크섬을 확인하기 전까지 미검증이다.
  - 진행할 것이 없는 호출(빈 입력이고 코덱에 남은 출력도 없음)은 코덱을 부르지 않고 {0, 0, true}를 돌려준다. 남은 출력이
    있으면 빈 입력으로도 부른다. 코덱에는 출력 공간을 항상 0보다 크게 넘기고, 입력을 다 넣었는데 출력 공간이 남았으면 빈
    입력으로 다시 부르지 않는다(zlib Z_BUF_ERROR·zstd 무진행 반복 오류 방지).
  - Deflate·Zlib·Brotli 스트림 뒤 byte는 출력 창 크기와 무관하게 그 byte를 받은 호출에서 거부한다.
- zlib 압축의 작은 출력 창: deflate는 flush 중 출력이 가득 차면 다음 호출에서 flush marker(빈 stored block)를 다시
  만든다. 창이 marker보다 작으면 TryFlush가 끝나지 않으므로(교차검증에서 발견), 남은 창이 64byte보다 작으면 64byte carry
  버퍼에 deflate한 뒤 나눠 넘긴다. 그래서 1byte 창으로도 flush·finish가 끝난다. 창이 정확히 차면 빈 marker가 한 번 더
  붙을 수 있으나 해제 결과는 같다.
- 실패: 코덱 실패·출력 성장 OOM·상한 초과는 Reset 전까지 같은 오류를 유지한다(sticky, 코덱 상태가 이미 입력을 소비했다).
  코덱을 건드리기 전의 거부(겹침, Finish 뒤 Process)는 상태를 바꾸지 않는다. Append 편의 함수는 그 호출에서 붙인 길이를
  되돌린다(일관성일 뿐 무결성 보장은 아니다).
- 메모리: zlib inflate 약 40KiB, deflate 약 256KiB, zstd 해제는 window + 블록 버퍼(Reset으로 해제되지 않고 객체 수명 동안
  유지). Brotli 해제는 출력에 맞춰 늘어나는 ring buffer와 Huffman 표이며 Reset이 해제기 상태를 새로 만들며 반환한다(13.4절). 이동 전용이며 코덱 상태는 heap에 고정하고, 이동된 뒤 호출은 StaleHandle이다. 한 객체는 한 번에 한 스레드만 쓴다.

## 11. 압축 사전 (3차, 교차검증 반영)

작은 입력(수십 byte–수십 KiB의 에셋·JSON·네트워크 메시지)이 많을 때 공통 내용을 미리 알려 압축률을 높인다. 헤더는
`Compression/CompressionDictionary.h`다. Godot `Compression`과 UE 5.6 `FCompression`·`FCompressedBuffer`에는 범용 압축의
사전 인자가 없다. UE에서 학습 사전을 쓰는 곳은 OodleNetwork 플러그인의 패킷 압축이며 학습은 commandlet(도구 단계)이
맡는다. 그래서 사용은 Core 런타임에, 학습은 도구 계층에 둔다(11.6절). 설계 초안은 교차검증 3건(zstd 1.5.7 원본·실측,
API·계약·형식, dictBuilder 조사)을 반영했다.

### 11.1 사전 객체

- `CompressDictionary`(zstd CDict, 압축 수준 고정)와 `DecompressDictionary`(DDict). 런타임 로더는 해제 사전만 만든다.
- 공유 handle이다(Core `SharedPtr`, 원자 참조 계수). 사전을 쓰는 문맥·스트림·블록 설정·reader·비동기 작업이 복사본을 가져
  수명을 보장한다. 사전 할당기는 마지막 handle을 놓는 스레드(비동기 worker 포함)가 해제하므로 다른 스레드 해제를 허용해야
  한다(System·Binned·Budget·Pool·External·Debug 가능, Arena·Stack 불가).
- 내용은 `ZSTD_dct_auto`다. magic 0xEC30A437로 시작하면 zstd 사전 형식(엔트로피 표·반복 offset·내용·사전 ID), 아니면 raw
  내용이다. 그래서 magic으로 시작하는 raw 내용은 형식 사전으로 해석되어 손상(InvalidArgument)이다. 형식 사전의 사전 ID가
  0이면 frame에 ID를 기록할 수 없어 raw와 구분되지 않으므로 InvalidArgument다. `Id()`는 생성된 CDict·DDict에서 읽는다
  (`ZSTD_getDictID_fromDict`는 검증 없이 필드만 읽는다).
- 크기 9byte–32MiB. zstd는 8byte 이하 내용을 색인하지 않고(압축기는 8byte 미만 사전을 조용히 무시한다), CDict는 사전 끝의
  2^max(hashLog+3, chainLog+1) byte만 색인하므로 큰 사전은 이득이 없다. 상한은 실수로 큰 버퍼를 넘기는 것을 막는다.
- 사전 byte는 복사한다(`ZSTD_dlm_byCopy`). byRef 사전은 원본이 사전 바로 뒤에 놓이면 연속 window로 처리되어 결과 byte가
  메모리 배치에 따라 바뀐다(교차검증 실측). 복사본이면 결정적이다.
- 식별: `Digest()`는 사전 byte 전체의 SHA-256(쿠킹 빌드 키·패키지의 사전 참조), `Id()`는 frame 헤더에 기록되는 zstd 사전
  ID(raw 0), `MemoryBytes()`는 준비된 사전과 상태의 크기다.
- OOM과 손상의 구분: zstd create 함수는 둘 다 NULL로 알린다. customMem opaque를 사전 상태 안의 `CodecMemory`로 두어 할당
  실패 flag를 기록한다(opaque는 해제 때도 쓰이므로 사전 상태가 CDict·DDict보다 오래 산다).
- 빈 handle(기본 생성·이동된 뒤)의 조회는 Fatal(`SharedPtr` 역참조와 같다), 소비 API에 넘기면 StaleHandle이다.
- LZ4(`LZ4_loadDict`)·zlib(`deflateSetDictionary`) 사전은 Unsupported(후속)다.

### 11.2 API 연동

- 일회성: `TryCompress`·`TryCompressAppend`·`TryDecompress`·`TryDecompressAppend`의 사전 overload. 사전 준비(CDict·DDict)는
  handle에 있어 호출마다 드는 비용은 사전 없는 함수와 같은 문맥 생성뿐이다.
- 문맥: `Compressor::TryCreate(CompressDictionary, allocator)`, `Decompressor::TryCreate(DecompressDictionary, allocator)`.
- 스트림: `CompressStream::TryCreate(CompressDictionary, allocator, sizeHint)`,
  `DecompressStream::TryCreate(DecompressDictionary, limits, allocator)`. Reset 뒤에도 같은 사전이다(세션 재설정은 참조
  사전을 유지한다). 사전 메모리는 window 상한과 별개다.
- 비동기: `TrySubmitCompress`·`TrySubmitDecompress`(Append·Span)의 사전 overload. 작업 capture가 handle 복사본을 가진다.
- `TryGetZstdFrameDictionaryId(frame)`: frame 헤더의 사전 ID(0 = 기록 없음, skippable 0). 로더·수신 측이 사전을 고를 때
  쓴다. `ZSTD_getDictID_fromFrame`은 여러 원인을 0 하나로 뭉치므로 `ZSTD_getFrameHeader`를 쓴다.
- `CompressionOutputRevision`(Compression.h): 이 모듈의 출력 byte를 바꾸는 결정(코덱 버전, frame 설정, 사전 매개변수 정책,
  블록 컨테이너 배치)이 바뀔 때마다 올린다. 압축 결과를 캐시하는 쪽은 형식·수준·사전 Digest(·블록 크기)와 함께 키에 넣는다.
  사전을 다시 학습하면 그 사전에 의존한 결과는 모두 다시 쿠킹된다.

### 11.3 수준 매개변수 정책

zstd 1.5.7에서 사용자 할당기를 받는 CDict 생성 함수(`ZSTD_createCDict_advanced`·`advanced2`)는 CDict를 "수준 없음"
(`ZSTD_NO_CLEVEL`, zstd_compress.c 5636)으로 만든다. 수준을 기록하는 `ZSTD_createCDict(L)`은 기본 malloc만 쓴다. 수준 없는
CDict를 그대로 쓰면 세 가지가 달라진다(교차검증 원본 추적).

1. `ZSTD_CCtx_init_compressStream2`가 frame 수준을 0(→ 수준 3 표)으로 덮어 windowLog가 수준 3 값이 된다.
2. 블록 분할기 판정도 수준 3 전략(dfast)으로 해 수준 16 이상에서 분할기가 꺼진다.
3. `ZSTD_compressBegin_internal`이 큰 입력(128KiB 이상이고 사전의 6배 이상)에서 그 수준의 전체 표로 사전을 다시 적재하는
   경로를 막고 항상 작은 입력용 CDict 표를 쓴다.

대책 없이 5.19MB 입력을 압축하면 표준보다 수준 9에서 59.5%, 수준 19에서 4.9% 크다(실측). 문맥마다 사전을 적재하는
localDict도 내부적으로 수준 없는 CDict라 같은 문제가 있어 기각했다. 채택한 방식은 이렇다.

- CDict: `ZSTD_createCDict_advanced(byCopy, dct_auto, ZSTD_adjustCParams(ZSTD_getCParams(L, 0, n), 513, n), customMem)`.
  `ZSTD_createCDict(L)`이 원본 크기를 모를 때 작은 원본(513byte)을 가정해 표를 줄이는 것(createCDict 모드)을 공개 API로
  재현한다.
- frame마다(세션 재설정 뒤, 원본 크기 고정 전): `ZSTD_CCtx_setCParams(ZSTD_getCParams(L, max(크기, 1), 사전 크기))`와
  `ZSTD_c_forceAttachDict` = (크기 ≥ 128KiB이고 크기 ≥ 6 × 사전 크기면 `ZSTD_dictForceLoad`, 아니면 기본 attach). 두 상수는
  zstd 내부(`ZSTD_USE_CDICT_PARAMS_SRCSIZE_CUTOFF`·`DICTSIZE_MULTIPLIER`)와 같아야 하므로 1.5.7 고정을 전제로 golden 시험이
  묶는다.
- 검증: 표준 경로(`ZSTD_createCDict(L)` + `ZSTD_CCtx_refCDict` + `ZSTD_compress2`)와 byte 비교 golden 100개가 같다. 형식 사전
  2178byte·raw 4KiB·raw 300KiB, 수준 1·3·5·9·13·16·19·20·22, 입력 0byte–2MB(copy 모드, 재적재 경계 6 × 사전 크기 ± 1 포함)다.
  golden은 공식 tarball 원본으로 만든 probe가 생성한다(`Tests/CompressionDictionaryTestData.h`에 방법 기록). 구현 교차검증은
  zstd 내부 함수를 직접 불러 CDict 매개변수 91,637건(수준 1–19 × 사전 9byte–32MiB)과 frame별 실효 상태(attach·copy·재적재
  경로, 매개변수 7개, 블록 분할기, LDM, row match finder) 1,367만 건이 표준과 같고, 실측 8,328건(raw·형식 사전 2.2KB–32MiB,
  입력 0–3.3MB)이 byte 단위로 같음을 확인했다. 사전 크기는 zstd도 형식 사전 전체 byte 수를 쓴다(`cdict->dictContentSize`).
- 1MiB를 넘는 입력은 1MiB 단위 stable 입력 호출(분할 단위)이라, 블록 분할기가 보는 경계가 `ZSTD_compress2` 단일 호출과 다를 수
  있다(이질적인 데이터에서 −0.28%–+2.26%, 사전과 무관한 기존 동작, 결정적). 비교는 같은 호출 패턴의 표준 경로와 한다.
- 스트림: 원본 크기를 모르므로 사전을 붙여(attach) 쓰고, 생성 때 `setCParams(ZSTD_getCParams(L, sizeHint, 0))`을 명시한다
  (attach 모드는 사전 크기를 매개변수 선택에 넣지 않는다). 공개 API의 호출 패턴(Process 반복 + Finish)은 수준 1–19에서 표준
  스트림과 같은 byte다(교차검증). 주의: 첫 호출이 입력을 가진 End면 zstd가 원본 크기를 정하는데 수준 없는 CDict라 재적재가
  막혀 결과가 달라진다(최대 +9.7%). `TryFinish`는 입력을 받지 않아 지금은 빈 스트림만 이 경로를 타며 결과가 같다. 입력을 받는
  Finish를 추가하면 frame 경로처럼 매개변수를 정해야 한다.
- 비용: 사전 문맥으로 큰 입력을 압축하면 사전 없는 수준 L과 같은 작업 메모리와 frame당 사전 재적재 비용(수준 19 약 5ms,
  수준 3 약 0.3ms)이 든다. 사전은 작은 입력에서 효과가 크고 큰 입력에서는 작다.

### 11.4 frame 사전 ID와 검사

- 사전 문맥(일회성·`Compressor`·스트림·블록 컨테이너)은 frame에 사전 ID를 기록한다(`ZSTD_c_dictIDFlag` 1). raw 사전은 ID가
  0이라 기록되지 않는다. 사전 없는 문맥은 기록하지 않는다(기존과 같다).
- 해제 문맥의 사전 ID와 다른 ID frame, 사전 없는 문맥에 ID가 있는 frame은 zstd가 `dictionary_wrong`으로 거부하고
  Unsupported로 보고한다.
- 형식 사전 문맥은 사전 ID 없는 frame(skippable 제외)을 Unsupported로 거부한다. zstd는 ID가 0인 frame을 검사 없이 사전으로
  푸는데, 형식 사전은 초기 반복 offset·엔트로피 표를 바꾸므로 checksum 없는 frame은 오류 없이 틀린 결과가 나온다(교차검증
  실측). 일회성 해제는 해제 전에 frame 헤더를 훑어 확인하고(헤더·frame 손상을 만나면 멈추고 해제가 보고), 스트림은 frame
  시작의 헤더 선파싱에서 window 검사보다 먼저 확인한다(오류가 호출자 상한 설정에 따라 바뀌지 않는다).
- raw 사전은 frame으로 불일치를 알 수 없다. checksum이 있으면 checksum 불일치(InvalidArgument)로 걸린다. raw 사전은 초기
  상태를 바꾸지 않아 사전 없이 압축한 frame도 그대로 풀린다.

### 11.5 블록 컨테이너 연동

- 헤더(9.1절) flags bit0과 사전 지문(사전 SHA-256 앞 8byte를 그대로 기록, little-endian 값으로 비교). 지문은 CRC를 통과한
  뒤의 우발적 사전 불일치를 잡는 용도라 64bit로 충분하다. zstd 사전 ID는 31bit이고 사용자 지정 ID는 충돌할 수 있으며 raw
  사전은 0이라 쓰지 않는다. 전체 Digest는 빌드 키·매니페스트에 둔다. 블록 frame은 사전 ID도 기록해 zstd가 블록마다 한 번 더
  확인한다.
- `CompressedBlocksSettings::TryMake(CompressDictionary, blockSizeLog2)`: 형식·수준은 사전이 정하고 설정이 handle 복사본을
  가진다. writer·순차·병렬 압축이 같은 byte를 만든다. 병렬 helper는 같은 CDict를 공유한다(읽기 전용, 스레드 안전).
- `CompressedBlocksReader::TryOpen(…, Span<const DecompressDictionary> 후보, allocator)`: 후보의 빈 handle은 파싱 전에
  StaleHandle이다. CRC 확인 뒤 지문이 같은 후보를 골라 handle을 가지며, 없으면 Unsupported다. 지문이 같은데 내용(Digest)이
  다른 후보가 둘 이상이면 InvalidArgument다(64bit 지문은 약 2^32 작업으로 맞출 수 있고, raw 사전은 frame에 ID가 없어 잘못
  고르면 검사가 없다). 사전을 쓰지 않는 컨테이너는
  후보를 무시하므로 범용 로더는 항상 패키지의 사전들을 넘겨도 된다. reader는 사전을 파싱하거나 만들지 않아 악성 컨테이너가
  사전 할당을 유도할 수 없다(stable output 해제는 사전과 무관하게 블록 입력 버퍼만 쓴다). `ZSTD_d_refMultipleDDicts`는
  refDDict마다 hash set을 할당하고 ID 없는 frame에 마지막 사전을 쓰므로 쓰지 않는다.
- 읽기 문맥은 사전과 무관하다. 블록을 해제할 때마다 reader의 DDict를 문맥에 걸고(`ZSTD_DCtx_refDDict`, O(1)) 해제 뒤 뗀다.
  그래서 한 문맥을 사전 있는·없는 reader에 번갈아 쓸 수 있고, 닫힌 reader의 사전을 가리킨 채 남지 않는다(주소 재사용으로
  다른 사전과 섞이지 않는다). 캐시 식별(reader id, 블록)은 그대로다. 병렬 읽기 helper도 같다.
- 헤더 배치를 바꾸며 version을 2로 올렸다. 40byte 헤더의 version 1 컨테이너는 손상이 아니라 Unsupported로 보고된다.
- 효과(시험): 레코드 600KB, 4KiB 블록, 수준 3에서 사전 컨테이너가 사전 없는 컨테이너보다 10% 이상 작다. 블록이 클수록
  이득은 줄어든다(기본 256KiB 블록보다 64KiB 이하 블록에서 쓴다).

### 11.6 학습 (4차, 교차검증 반영)

모듈 `Core/Source/Developer/CompressionTraining`(Core 계층 개발 도구, 헤더 `CompressionTraining/CompressionDictionaryTraining.h`)
이다. 쿠커·도구가 링크하고 게임 런타임은 링크하지 않는다(UE에서 사전 학습을 OodleNetwork commandlet이 맡는 것과 같은
배치). zdict.h를 include하는 유일한 adapter다. 설계 초안은 dictBuilder 조사 결과와 원본·실측 교차검증(예외 경로 충돌
재현 포함)을 반영했다.

- 링크 묶음: `lib/dictBuilder`의 cover.c·fastcover.c·zdict.c·divsufsort.c와 `lib/common/pool.c`는 서로 참조해 떼어낼 수 없다
  (fastCover와 finalize만 써도 divsufsort가 필요하다). threading.c·debug.c는 필요 없다. 같은 Zstd 모듈 manifest에 넣었다
  (별도 모듈은 폴더당 모듈 하나·'..' 금지 규칙 때문에 헤더 약 40개를 복제해야 하고 내부 구조체 배치에 묶여 버전 동기화
  위험이 크다). 정적 라이브러리라 학습을 부르지 않는 게임 바이너리에는 들어가지 않는다.
- 라이선스: divsufsort.c·.h는 MIT(Yuta Mori)이고 zstd LICENSE에 포함되지 않아 별도 고지가 필요하다. manifest의 `license`를
  모든 manifest에서 `licenses` 배열로 바꿨다(2026-11-12 rocks-62와 합의: 항목마다 `licenseSource` 선택, InstalledSdk의 데이터
  라이브러리 라이선스 복사는 `embeddedIn` 모듈이 SDK에 있을 때만 하는 동작 유지). Zstd manifest는 `divsufsort.h`(MIT 원문
  전체가 머리 주석에 있다)를 두 번째 항목으로 둔다(3.2절).
- 할당(승인된 좁은 예외, 2026-11-13): dictBuilder는 malloc·calloc을 직접 부르고(약 30곳) customMem을 받지 않으며, 내부
  CCtx·CDict도 기본 메모리를 쓴다. 원본을 고치지 않고 할당기를 바꿀 방법이 없어 학습 시점의 CRT 할당만 예외로 둔다. 결과
  사전은 output의 할당기로 저장한다. 학습 작업 메모리는 대략 10MiB + 4 × capacity + 압축 수준별 문맥이다(capacity 110KiB에서
  수준 3 약 12MiB, 수준 19 약 15MiB, 실측).
- `TryTrainCompressionDictionary(표본, 표본 길이들, settings, output) → {bytes, segmentSize, dmerSize}`:
  - `ZDICT_optimizeTrainFromBuffer_fastCover` 하나만 쓴다(`ZDICT_trainFromBuffer_fastCover`는 calloc NULL 검사가 빠져 OOM에서
    죽고, `ZDICT_trainFromBuffer`는 압축 수준이 3으로 고정이라 수준 19에서 쓸 사전이 약 8% 나빠진다). k를 지정하면 탐색 범위가
    그 값 하나라 정확히 1회다. k가 0이면 [50, 2000]에서 후보 5개(steps 4, `ZDICT_trainFromBuffer`와 같다)를 시험한다. steps
    40은 8–16배 느린데 이득이 0–0.5%였다. d는 8(6 선택 가능, 둘 다 탐색은 시간만 두 배이고 매번 8이 뽑혔다), f 20, accel 1,
    shrinkDict 0, nbThreads 1, splitPoint 1.0(모든 표본으로 학습·평가)이다. 고른 k·d를 돌려주며 그 값으로 다시 학습하면 같은
    사전이다.
  - 호출 전에 거르는 인자 규칙(InvalidArgument): 표본 5개 이상·2^32 − 2개 이하(fastCover가 표본 수 + 1을 32bit로 계산한다), 길이 합 = 표본 byte 수, 8 ≤ 합 < 2^32 − 1,
    capacity 256(ZDICT_DICTSIZE_MIN) – 32MiB, 압축 수준 1–22, d ∈ {6, 8}, k = 0이거나 d ≤ k ≤ capacity이고 k − d + 1 ≤ 65535,
    사전 ID 0 또는 [32768, 2^31 − 1](zstd 예약 범위 밖), 표본이 output 저장소와 겹치지 않음. upstream은 d ∉ {6, 8}·k >
    capacity를 원인 없는 GENERIC으로 알리고, splitPoint < 1에서 학습 부분 합이 8byte 미만이면 프로세스가 죽는다(실측:
    접근 위반·0 나누기). splitPoint를 1.0으로 고정해 그 경로를 없앴다.
  - 오류 대응: parameter_outOfBound·srcSize_wrong·dstSize_tooSmall InvalidArgument, memory_allocation OutOfMemory, GENERIC
    (모든 후보 실패) DependencyFailed(후보 안의 할당 실패도 GENERIC으로 올 수 있다). finalize의 dstSize_tooSmall은
    래퍼가 충분히 확보하므로 upstream 헤더 버퍼(256byte, 이론상 최대 297byte) 초과라 DependencyFailed다.
  - 결과 크기는 대개 capacity와 같다. fastCover가 사전 끝부터 채운 뒤 finalize가 헤더 자리만큼 앞부분을 남기고 끝을 자른다
    (upstream 동작, 결정적).
- `TryFinalizeCompressionDictionary(내용, 표본, 표본 길이들, settings, output)`: 호출자가 정한 내용에 엔트로피 표·반복
  offset·사전 ID를 붙인다(`ZDICT_finalizeDictionary`). capacity를 받지 않고 헤더(최대 256byte) + max(내용, 8)을 확보해 내용을
  자르지 않는다(zstd는 용량이 모자라면 헤더 정의와 달리 내용의 끝을 자른다). 표본은 엔트로피 통계에만 쓴다(0개도 된다).
- 스레드: fastCover는 전역 표시 수준을 동기화 없이 쓰므로 학습 호출만 해제하지 않는 `platform::Mutex`로 직렬화한다(종료
  중 학습해도 소멸된 mutex를 쓰지 않는다. POSIX mutex 소멸자는 잠긴 상태에서 Fatal이다). fastCover가 부르는 COVER 보조 함수는
  전역을 쓰지 않고, COVER의 qsort 전역은 fastCover 경로에 없다. finalize는 전역 상태가 없어 잠그지 않는다. 취소·진행률은
  dictBuilder가 제공하지 않는다. 표시 수준은 0이다.
- 사전 ID: 0이면 내용의 XXH64에서 유도한다(0이 되지 않으며 엔트로피 표·수준·표본은 반영하지 않는다). 정체성은 Digest와
  RCBK 지문이 맡고 frame의 ID는 해제 대조용이라 충돌해도 그 검사만 약해진다. 같은 내용으로 수준만 다른 사전을 만들면 ID를
  다르게 준다.
- 결정성: 같은 표본(순서 포함)·설정이면 같은 byte다. fastCover·finalize 출력이 Windows x64와 Android x86_64에서 같다(실측,
  COVER는 qsort 구현에 따라 다르다). 표본 순서는 위치·epoch 순회에만 영향을 줘 일부 순열에서만 결과가 바뀐다.
- 시험(`Tests/CompressionDictionaryTrainingTests.cpp` 8개, Windows x64 Debug·ASan): finalize가 사전 사용 시험 자료
  `FormattedDictionaryA`(probe가 공식 원본으로 만든 사전)를 byte 단위로 재현, finalize 계약(유도 ID 범위, 내용 비자름, 빈
  입력, 인자 오류 시 output 불변, 겹침), 학습 결정성 golden·탐색 결과 재현, 학습 사전 효과(레코드 40개: 사전 없음 6331byte,
  같은 크기 raw 사전 3569byte, 학습 사전 2961byte), 인자 규칙(upstream이 죽거나 GENERIC인 경우, 길이 합 overflow·상한 포함)·0byte 표본,
  output OOM, 6개 worker 동시 학습 = 순차 결과.
- 후속: 학습한 내용을 수준별로 다시 finalize하는 보조 함수(`ZDICT_getDictHeaderSize`로 내용 위치를 얻는다), 메모리 추정
  함수, macOS ARM64·Android arm64 실기기 결정성 확인.

### 11.7 시험

사전 시험 27개(Windows x64 Debug·ASan 통과). 세 파일로 나뉜다. 공용 도구(결정적 레코드 생성기·시험 사전·생성 도우미)는
`Tests/CompressionDictionaryTestSupport.h`, 시험 자료(형식 사전 A, 표준 경로 golden 100개)는 `Tests/CompressionDictionaryTestData.h`다.

- `CompressionDictionaryTests.cpp`(9): 생성 계약(형식·크기 경계·사전 ID·Digest·손상·magic raw·ID 0·handle·모든 소비 API의
  빈 handle), 표준 수준 의미 golden 100개와 자유 함수 = 문맥·왕복, 작은 입력 압축률(레코드 40개, 5575byte 대 9760byte),
  frame 사전 ID 검사(조회·불일치·ID 없음·섞인 연결 frame·raw 사전 checksum), 스트림 왕복·Reset·거부, handle 수명(임시
  handle만 준 문맥·스트림·블록 설정·reader), 4 worker 비동기 공유 = 동기, 사전·문맥·스트림 생성 OOM 분류, 블록 컨테이너
  (헤더 bit·지문, 병렬 = 순차, 압축률, 후보 선택, 문맥 공유, 지문 손상 대 다시 맞춘 지문, golden).
- `CompressionDictionaryEdgeTests.cpp`(9, 교차검증 시험 이전): 스트림 헤더를 1·2·3·5·7·13byte로 나눠 넣기와 skippable
  frame, 사전 ID 검사가 window 검사보다 먼저임, 값이 0인 사전 ID 필드, 일회성 frame 순회(크기 미기록 frame 뒤 ID 없는
  frame, 잘린 꼬리), Span 비동기 해제, 수준이 사전의 일부임(1 대 19), writer = 일회성 = 병렬(난수 구간이 섞여 raw 저장과
  압축 블록이 함께 있는 입력, 재적재 경로 포함), 블록 일부만 걸치는 1MiB 초과 병렬 읽기·인덱스만 여는 흐름·verify를 끈
  손상 블록 뒤 같은 문맥 재사용, 블록 사전 ID 위조·raw 사전 컨테이너·version 1 레이아웃(Unsupported).
- `CompressionDictionaryRobustnessTests.cpp`(9, 교차검증 시험 이전): 사전 상태 주소 재사용을 강제한 reader·사전 교체 중 문맥
  재사용, 8 스레드의 CDict·DDict·reader 공유, 압축 크기 1MiB를 넘는 블록(zstd 스트리밍 경로) 뒤 사전 없는 컨테이너, 블록
  컨테이너 OOM sweep, raw·형식 사전 교차·블록 frame 바꿔치기 분류, 인덱스만 여는 파일 흐름, 해제된 사전 뒤 문맥 재사용
  (ASan), 이동된 블록 설정의 사전 유지, window 2^32 frame의 사전 ID 조회(Unsupported).
- 사전 밖 회귀: window가 구현 상한을 넘는 zstd frame을 일회성 해제(크기를 아는·모르는)에 넣으면 스트림과 같이
  Unsupported다(`CompressionTests.cpp`). 전에는 `ZSTD_findDecompressedSize`가 원인을 하나로 뭉쳐 InvalidArgument였다.

## 12. LZ4 frame (5차)

기준일 2026-11-13. `CompressionFormat::Lz4Frame`은 LZ4 Frame Format 1.6.1(`lz4` CLI·`.lz4` 파일과 호환)이다. `Lz4` raw block은
원본 크기·체크섬·끝 표지가 없어 스트리밍과 무결성 검사를 할 수 없다. frame은 블록 헤더·endmark·content checksum을 더해 둘 다
제공하고, 그 대가로 해제가 raw block의 60%대다(12.5절). legacy frame(`lz4 -l`, magic 0x184C2102)은 지원하지 않는다
(Unsupported). raw block은 블록 컨테이너(RCBK)의 LZ4 블록 형식으로 그대로 둔다(컨테이너가 크기·CRC를
따로 가진다). enum 값은 끝에 덧붙였다(프로세스 안 식별용이며 영속 값이 아니다). 구현은 `Private/Lz4FrameCodec.cpp`다.

### 12.1 frame 설정

| 항목 | 값 | 근거 |
|---|---|---|
| 블록 최대 크기 | 64KiB(`LZ4F_max64KB`) | linked 블록은 앞 64KiB를 참조하므로(LZ4 offset 상한과 같다) 4MiB 블록과 압축률이 거의 같다. 블록 헤더는 64KiB당 4byte(0.006%)다. 해제 문맥은 약 256KiB로 작다(4MiB 블록이면 약 8.1MiB) |
| 블록 방식 | linked | 독립 블록은 블록마다 history를 잃어 압축률이 떨어진다. 블록 단위 임의 접근은 RCBK가 맡는다 |
| content checksum | 켬(XXH32) | 무결성. zstd checksum·gzip CRC와 같은 역할이다 |
| 블록 checksum | 끔 | content checksum이 같은 손상을 잡고, 블록마다 4byte와 XXH32 비용이 든다. 해제는 블록 checksum이 있는 frame도 확인한다 |
| 원본 크기 | 일회성만 기록 | 해제가 출력을 한 번에 확보한다. 스트림은 크기를 모른다 |
| autoFlush | 일회성 1, 스트림 0 | 아래 참고 |
| favorDecSpeed | 0 | 수준 10–12는 압축률 우선 |
| 수준 | {1, 12, 1}, Rocks 수준 = LZ4F `compressionLevel` | lz4frame.c는 2 미만(`LZ4HC_CLEVEL_MIN`)을 fast(acceleration 1), 2를 lz4mid, 3–9를 HC, 10–12를 optimal parser로 쓴다. `Lz4`와 같은 의미다 |
| nbWorkers | 0(`LZ4F_MULTITHREAD` 미정의) | 스레드 코드가 컴파일되지 않는다. 병렬은 RCBK·비동기 작업이 맡는다 |
| 사전 ID | 0 | 사전은 zstd만 지원한다 |

autoFlush는 처음 결정(일회성·스트림 모두 0)에서 일회성만 1로 바꿨다. frame 형식은 같고 압축기 동작만 다르다.

1. `TryGetCompressBound`(= `LZ4F_compressFrameBound`)는 autoFlush 1 기준이다. autoFlush 0이면 1MiB 조각의
   `LZ4F_compressBound`(블록 하나를 더 센다)가 bound 크기 출력의 남은 공간보다 커서, 조각마다의 성공이 LZ4F 계약 밖(1.11의
   "출력이 작아도 시도" 동작)에 기댄다. autoFlush 1이면 bound 크기 출력에서 모든 `LZ4F_compressUpdate`가 계약으로 성공한다.
2. 마지막 부분 블록도 입력 위치에서 바로 압축한다. autoFlush 0은 그 블록을 LZ4F 내부 버퍼로 복사해 압축하므로 history가
   외부 dictionary가 되고, 결과가 두 버퍼의 주소 배치에 영향받을 여지가 있다. autoFlush 1(`stableSrc`)은 1MiB 조각이 앞 조각
   바로 뒤에 이어져 한 번에 넣은 것과 같고, 64KiB를 넘는 입력에서 upstream `LZ4F_compressFrame`과 byte 단위로 같다(probe로
   수준 1·2·9·12 확인, 시험 golden이 그 값이다).
3. 내부 블록 버퍼가 64KiB 작다(128KiB).

### 12.2 할당과 빌드

- 문맥은 `LZ4F_createCompressionContext_advanced`·`LZ4F_createDecompressionContext_advanced`(`LZ4F_STATIC_LINKING_ONLY`,
  v1.9.4+)에 Allocator 경유 alloc·free를 넘겨 만든다(calloc은 비워 두면 LZ4F가 alloc 뒤 0으로 채운다). 문맥·LZ4 상태·블록
  버퍼·flush 단계 버퍼 등 LZ4F의 모든 할당이 이 함수를 거친다.
- 기본 할당 경로: lz4frame.c는 사용자 할당기가 없으면 기본 할당 함수를 쓴다. 이 경로를 타는 함수는
  `LZ4F_compressFrame`(HEAPMODE 0에서도 HC 상태를 기본 할당기로 잡는다)·`LZ4F_createCompressionContext`·
  `LZ4F_createDecompressionContext`·`LZ4F_createCDict`이며 adapter는 부르지 않는다. 저장소 안에서 이를 강제하려고
  `LZ4_USER_MEMORY_FUNCTIONS`(upstream 설정 지점)로 기본 경로를 `Lz4Unity.c`의 `LZ4_malloc`·`LZ4_calloc`·`LZ4_free`에 연결했다.
  이 함수들은 NULL을 돌려주므로 실수로 기본 경로를 타면 CRT 할당 대신 `allocation_failed`가 되고 시험에 드러난다. 그래서
  lz4frame.obj는 CRT 할당 함수를 참조하지 않는다. 남는 CRT 호출은 xxhash.c의 `XXH32_createState`·`XXH64_createState`(사용자
  할당 설정 지점이 없다)뿐인데 lz4frame.c는 XXH 상태를 문맥 안에 두고 이 함수를 부르지 않는다. probe에서도 같은 감시
  함수로 adapter와 같은 호출 순서(일회성·스트림·flush·해제)가 기본 경로를 0회 탐을 확인했고, 시험은 계수 allocator로 모든
  할당이 주입 할당기를 거치고 해제 뒤 0인지 확인한다.
- 번역 단위: `lz4frame.c`·`xxhash.c`는 `Lz4Unity.c`에 합치지 않고 manifest에 따로 둔다. upstream 합본(`cat lz4.c lz4hc.c
  lz4frame.c`)에서 lz4frame.c는 lz4.c의 `ALLOC` 매크로를 이어 쓰는데, `LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION`이
  그 매크로를 컴파일 오류 표지로 바꾸므로 합칠 수 없다(upstream도 이 정의에서는 LZ4F를 지원하지 않는다고 적는다). 따로
  컴파일한 lz4frame.c는 자기 할당 매크로를 쓰고 lz4.c에서 제거된 할당 API(`LZ4_createStream` 등)를 부르지 않으므로 정의를
  유지한 채 빌드된다. xxhash.c는 upstream 합본에서도 별도 번역 단위다.
- xxHash 심볼: LZ4는 xxHash 0.6.5를 들고 있다. `XXH_NAMESPACE=LZ4_`로 심볼을 `LZ4_XXH32` 등으로 바꾼다(llvm-nm 확인). zstd는
  자기 `xxhash.h`에서 `XXH_NAMESPACE`를 `ZSTD_`로 정하므로 두 라이브러리를 함께 링크해도 겹치지 않고, 앱이 다른 xxHash를
  링크해도 겹치지 않는다. `XXH_PRIVATE_API`(static inline)도 가능하지만 upstream이 충돌 회피용으로 문서화한 방식은 namespace다.
- `LZ4FLIB_VISIBILITY=`(라이브러리·adapter), `LZ4F_STATIC_LINKING_ONLY`(adapter만. lz4frame.c는 스스로 정의한다).
  `lz4frame_static.h`는 `LZ4F_STATIC_LINKING_ONLY`를 정의하고 lz4frame.h를 포함하는 호환 헤더라 consumerDefines로 대신하고
  들이지 않는다(컴파일하지 않는 파일은 넣지 않는다).
- 진단 지시문: `lz4frame.c` 49행·`xxhash.c` 124행의 `#pragma warning(disable : 4127)`(`_MSC_VER` 조건)을 manifest에 기록했다.
  지시문을 지운 사본도 clang-cl `/W4 /WX`(`/Od`·`/O2`)와 Android NDK clang `-Wall -Wextra -Wpedantic -Werror`(`-O0`·`-O2`)에서
  경고 0이다(`native`의 `ThirdPartyDirectivesHideNoWarnings`가 매 실행 확인).

### 12.3 일회성 계약

- bound: 헤더 19 + 4 × ⌈n / 64KiB⌉ + n + 8(endmark·checksum)이다. 계산 전에 overflow를 거른다. `Lz4MaxInputBytes` 한계가 없다.
- 압축: `LZ4F_compressBegin`(원본 크기 기록) → 1MiB 조각 `LZ4F_compressUpdate`(`stableSrc`, 조각마다 취소 확인) →
  `LZ4F_compressEnd`다. 출력이 bound보다 작으면 시도하고 넘치면(헤더·블록·footer 어디서든) CapacityExceeded다. footer가 남은
  공간을 정확히 채우면 LZ4F는 출력이 남았을 수 있다고 보므로 1byte 예비 창으로 끝을 확인한다. 그래서 정확히 결과 크기인
  출력은 성공한다. 실패·취소 뒤 남은 frame은 다음 `LZ4F_compressBegin`이 버리므로 재사용 문맥도 새 문맥과 같은 byte다.
- 해제: 연속 frame과 skippable frame(magic 0x184D2A5X)을 모두 처리한다. frame 시작마다 magic을 확인하고(아니면
  InvalidArgument, legacy frame은 Unsupported) 헤더 전체(`LZ4F_headerSize`, 7–19byte)만 LZ4F에 먼저 넣어 version·예약 bit·블록
  크기·헤더 checksum을 검증한다. 검증을 통과한 frame이 사전 ID를 기록했으면 Unsupported다(zstd 사전 ID frame·zlib FDICT와 같은
  분류). 헤더를 먼저 검증하므로 FLG가 손상된 frame은 사전 frame이 아니라 InvalidArgument다(독립 검토에서 발견해 고쳤다). 입력·출력을 1MiB 단위로 넣으며 취소를
  확인한다. 출력이 차면 1byte 예비 창으로 계속한다. 1byte라도 나오면 Span은 CapacityExceeded, Append는 출력을 늘린다(상한이면
  CapacityExceeded). frame 끝(endmark·checksum)만 남았으면 출력 없이 끝나므로 정확히 상한 크기인 결과는 성공한다. 입력을 다
  넣었는데 frame이 끝나지 않았고 출력 공간이 남았으면 절단(InvalidArgument)이다. frame 뒤 쓰레기는 magic 검사나 절단으로
  InvalidArgument다.
- Append 성장: 첫 frame에 기록된 원본 크기(없으면 64KiB)를 min(상한, 입력 × 256 + 64KiB)만큼 먼저 확보하고, 이후에는 예비
  창에 byte가 나올 때만 두 배씩(최소 64KiB) 늘린다. output 길이는 base + 상한을 넘지 않고, 2^40byte를 선언한 30byte frame도
  입력에 비례한 만큼만 먼저 잡는다. Span 해제는 `stableDst`(이전 출력이 다음 창 바로 앞에 그대로 있다)로 history 복사를
  생략한다. Append는 성장 재할당 때문에 쓰지 않는다(LZ4F가 history를 내부 버퍼로 복사한다).
- 무결성: content checksum(있으면 frame 끝), 블록 checksum(있으면 블록마다), 기록된 원본 크기(frame 끝, `frameSize_wrong`)를
  확인한다. 원본 크기 필드는 출력 확보 힌트로만 쓰고 믿지 않는다. 헤더 checksum까지 다시 맞춘 거짓 크기도 InvalidArgument다.
- 오류 대응: `allocation_failed` OutOfMemory, 압축의 `dstCapacity_tooSmall` CapacityExceeded, API 계약 오류(parameter·
  stage·io 계열) DependencyFailed, 그 밖의 해제 오류(헤더·블록·checksum·크기·GENERIC) InvalidArgument다. 빈 해제 입력은
  InvalidArgument다.
- 메모리(계수 allocator로 시험이 상한을 묶는다): 압축 문맥 수준 1 약 145KiB, HC(2–12) 약 450KiB(블록 버퍼 128KiB + LZ4
  상태), 해제 문맥 약 256KiB(64KiB linked 블록). 첫 frame에서 할당하고, 재사용 문맥은 같은 설정이면 다시 할당하지 않는다.

### 12.4 스트림

- 압축: `TryProcess`는 입력을 64KiB − 1 이하 조각으로 `LZ4F_compressUpdate`에 넣는다. 블록보다 작게 넣으면 LZ4F가 블록을
  항상 내부 버퍼에서 완성해 압축하므로 결과 byte가 입력 내용과 `TryFlush` 위치로만 정해진다(입력 조각·출력 창 크기·단계 버퍼
  경유와 무관). 10절의 일반 계약("같은 호출 순서일 때만")보다 강하다. probe에서 upstream에 1byte–1MiB 조각으로 넣어도 같은
  byte였고, 시험이 출력 창 7–70000byte·입력 조각 1byte–128KiB로 확인한다.
- 출력 대상: LZ4F 호출 한 번의 최악 출력(헤더 19byte, update는 `LZ4F_compressBound(조각)` = 65548byte, flush·end는
  `LZ4F_compressBound(0)` = 65548byte) 이상의 창이면 호출자 출력에 바로 쓰고, 아니면 65548byte 단계 버퍼에 쓴 뒤 나눠
  넘긴다(zlib carry와 같은 방식). 최악 크기 이상을 주므로 LZ4F 호출은 출력 부족으로 실패하지 않는다(실패하면 frame이 무효가
  된다). flush·end는 한 번에 끝나서 LZ4F 자체의 flush 단계 버퍼는 쓰이지 않는다. 그래서 1byte 창으로도 헤더·블록·flush·
  finish가 유한한 호출 안에 끝난다.
- `TryFlush`는 `LZ4F_flush`, `TryFinish`는 `LZ4F_compressEnd`다. 단계 규칙은 10절과 같다(단계 버퍼를 넘기는 중이면
  Flushing·Finishing). `Reset`은 단계 버퍼를 비우고, 다음 호출의 `LZ4F_compressBegin`이 진행 중인 frame을 버린다. frame에 원본
  크기를 기록하지 않는다(헤더 7byte). 입력 없이 끝낸 스트림은 15byte 빈 frame이다.
- 해제: `LZ4F_decompress`가 부분 입력·출력을 직접 처리한다(반환값은 다음 입력 크기 힌트이고 0이면 frame 끝이다). frame
  시작에서 magic·FLG 5byte로 헤더 크기를 알고 헤더 전체(최대 19byte)를 모아 일회성과 같이 LZ4F로 먼저 검증한다. 쓰레기는 받은
  byte에서 바로 InvalidArgument, legacy frame과 사전 ID frame은 Unsupported다. frame 뒤 byte는 다음 frame이며 skippable frame만 받아도 `TryFinish`는 성공이다. 출력 누계 상한·
  sticky 실패·경계 판정은 10절과 같다. `maxWindowLog`는 범위만 검사한다(작업 메모리는 frame 헤더의 블록 크기가 정한다).
- 메모리: 압축 스트림 수준 1 약 270KiB, HC 약 580KiB(블록 버퍼 192KiB + 단계 버퍼 64KiB + LZ4 상태), 해제 약 256KiB(4MiB
  블록 frame은 약 8.1MiB). Reset으로 해제되지 않고 객체 수명 동안 유지한다.

### 12.5 처리량

`CompressionBench --size 16`(합성 16MiB, Release, Windows x64, 같은 기기 1회, 단일 스레드)이다.

| 형식 | 수준 | 압축 비율 | 압축 MB/s | 해제 MB/s |
|---|---|---|---|---|
| Lz4(raw block) | 1 / 9 | 0.742 / 0.600 | 1320 / 23.4 | 5702 / 6064 |
| Lz4Frame | 1 / 9 | 0.740 / 0.600 | 1006 / 23.1 | 3619 / 3949 |
| Lz4Frame 스트림(64KiB 조각) | 1 | 0.740 | 948 | 2679 |

frame 해제가 raw block보다 느린 차이는 대부분 XXH32 content checksum 계산이다. upstream probe에서 checksum 확인만 끈
(`skipChecksums`) frame 해제는 raw block 해제와 같은 속도였고, XXH32는 이 기기에서 약 8GB/s였다. 압축률은 64KiB linked 블록이라
raw block과 거의 같다. 스트림은 64KiB 조각마다 LZ4F 내부 버퍼를 거치므로 일회성보다 느리다.

### 12.6 시험

`Tests/Lz4FrameTests.cpp` 24개(Windows x64 Debug 약 2.0초, ASan 약 3.8초). 독립 자료는 공식 tarball 원본만으로 빌드한
probe(저장소 밖)가 만들었다.

- 수준·bound·쓰지 않는 용도: 수준 범위, bound 식과 overflow, 사전 생성 Unsupported, 블록 컨테이너 설정·읽기 문맥
  InvalidArgument, 빈 해제 입력·겹침
- 왕복: 0·1·1000·64KiB·64KiB+1·1MiB+5byte × 수준 1·2·9·12, Span·Append·문맥 재사용, 정확한 크기 출력의 압축·해제 성공과
  1byte 부족 CapacityExceeded, 원본 크기 기록
- 재사용 문맥: 헤더·블록·footer 단계의 출력 부족 뒤에도 자유 함수와 같은 byte
- 결정성 golden: 일회성 1.5MiB 수준 1·2·9·12(upstream `LZ4F_compressFrame`과 같은 값), 스트림 1.5MiB 수준 1·9(4099byte 조각과
  중간 flush, 큰 조각·Reset 재사용도 같은 byte), 출력 창·입력 조각을 바꿔도 같은 byte
- upstream 호환: upstream 기본 설정 frame(checksum·크기 없음, 독립 블록) 해제, adapter 결과 = upstream이 같은 설정으로 만든
  frame, 4MiB 블록·독립 블록·블록 checksum·비압축 블록이 섞인 upstream frame 해제(1byte 스트림 포함), 블록 checksum 손상,
  사전 ID frame Unsupported
- 연속·skippable frame, 출력 상한의 합산, skippable만 있는 입력, 빈 frame
- 손상·절단·뒤 byte: 모든 길이 절단, 쓰레기·0·magic 앞부분·skippable 앞부분, checksum·헤더 checksum·magic·예약 bit 손상,
  고정 seed bit flip 600회(실패하거나 원본과 같음)
- 원본 크기 검사: 헤더 checksum을 다시 맞춘 거짓 크기(66·68), 2^40 선언 frame의 메모리 상한
- 폭탄: 16MiB 0을 1MiB 상한으로 해제(일회성·스트림, 최고 사용량 < 2MiB)
- 취소 probe: 압축 입력 1MiB·해제 1MiB마다, Append 길이 복원, 확인 횟수(4MiB의 0을 풀 때 출력 1MiB마다 확인)
- 비동기 = 동기(Append·Span, 의존 연결)
- OOM: 할당 시도 위치마다(일회성 압축, 크기를 아는·모르는 frame의 Append 해제와 그 성장, Span 해제, 스트림 생성·쓰기·끝),
  누수 0, 길이 복원, sticky·Reset 복구
- 할당 경로: 계수 allocator, 재사용 문맥의 무할당, 문맥 메모리 상한
- 헤더 검증: FLG 사전 ID bit 손상(일회성·스트림 frame)·version 손상은 InvalidArgument, 헤더가 유효한 사전 ID frame과 legacy
  frame(단독·정상 frame 뒤)은 Unsupported
- 스트림: 모든 조각 나누기(1·7·4096byte·1MiB) 왕복, 수준 2·9·12의 조각 무관성, 1byte 창 역압·종료(Append와 같은 byte),
  flush 뒤 접두 해제(1byte 창 포함), 단계 규칙, 누계 상한·sticky·Reset, 1MiB를 넘는 창의 Process·Flush·Finish, 정확한 상한의
  TryProcess, 상한 뒤 빈 frame·1byte frame, 여러 번 늘린 뒤 길이 복원, 상한 SIZE_MAX, 이동 대입, 창 = 원본 크기의 한 호출
  완료, 입력·출력 겹침, 이동된 객체, 절단·경계·쓰레기·magic 앞부분·1byte skippable

### 12.7 후속

- macOS ARM64·Android 실기기 실행(결정성 golden 포함).
- LZ4·LZ4 frame 사전(`LZ4F_compressBegin_usingCDict`), 독립 블록 선택 설정은 수요가 생기면 다룬다.

## 13. Brotli 해제 (6차)

기준일 2026-11-13. `CompressionFormat::Brotli`는 Brotli stream(RFC 7932)의 **해제 전용** 형식이다. 용도는 WOFF2 글꼴(W3C
WOFF File Format 2.0, 글꼴 표 전체가 Brotli stream 하나)과 HTTP `Content-Encoding: br`로 받은 자산이다. Godot
`Compression::MODE_BROTLI`도 해제만 지원한다(Godot parity). Rocks가 Brotli를 쓰는 쪽은 아니어서 압축기는 들이지 않았다. enum
값은 끝에 덧붙였다(프로세스 안 식별용이며 영속 값이 아니다). 구현은 `Private/BrotliCodec.cpp`다.

### 13.1 선정·버전·출처

- 라이브러리는 google/brotli 1.1.1(2026-10-21)이다. 처음 지시는 1.1.0이었으나 1.1.1로 바꿨다. 1.1.1은 해제기 보안 수정 하나와
  버전 표기만 더한 patch release다. upstream CHANGELOG의 CVE-2026-111497이 그 수정이며, 잘못된 block switch를 오류로 보고하지
  않아 범위 밖 쓰기가 가능했다(커밋 da98063 "Detect and report invalid stream earlier"). 공개 API는 같고 `decode.h`에 오류
  코드 `BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH`(-17)가 추가됐다. 1.1.0 archive와 비교하면 `CHANGELOG.md`·
  `c/common/version.h`·`c/dec/decode.c`·`c/include/brotli/decode.h` 4개만 다르다.
- 출처는 GitHub tag archive(`v1.1.1.tar.gz`)다. sha256은 `1a84dfaa…20da8`이고 전체 값은 manifest에 있다. 릴리스 자산 digest와
  upstream 서명은 없다. 그래서 GitHub API로 받은 tag commit(`db0631d…`)의 tree 목록과 archive 135개 파일의 git blob hash가
  모두 같음을 확인했다. 나머지 파일은 upstream `.gitattributes`의 export-ignore라 archive에 없다. 비교한 1.1.0 archive의
  sha256은 Homebrew formula 값과 같다.
- 라이선스는 MIT(`brotli-1.1.1/LICENSE`)이고 바이너리 배포 시 고지가 필요하다(`binaryNoticeRequired`).
- 해제기만 들였다.
  - 번역 단위 10개: `c/common`의 `constants`·`context`·`dictionary`·`platform`·`shared_dictionary`·`transform`.c, `c/dec`의
    `bit_reader`·`decode`·`huffman`·`state`.c
  - 헤더: 그 내부 헤더와 `c/include/brotli`의 `decode.h`·`port.h`·`shared_dictionary.h`·`types.h`. 이 4개는 adapter 헤더다.
  - 넣지 않은 것: 압축기(`c/enc`, `encode.h`), 도구(`c/tools`), 다른 언어 binding, 시험 자료
  - 내장 사전(`dictionary.c`, 122,784byte, 약 120KiB)은 정적 데이터다.

### 13.2 할당과 빌드

- 해제기 상태는 `BrotliDecoderCreateInstance(alloc_func, free_func, opaque)`에 Allocator 경유 함수 두 개를 모두 넘겨 만든다.
  해제기 상태, shared dictionary 상태, ring buffer, Huffman 표, context map, block type 표 등 upstream의 모든 할당이 이 함수를
  거친다.
- CRT 경로는 오브젝트에 남는다. 할당 함수가 없을 때 쓰는 분기는 다음과 같다:
  - `platform.c`의 `BrotliDefaultAllocFunc`·`BrotliDefaultFreeFunc`와 이를 고르는 `state.c`의 `BrotliDecoderStateInit` 분기
  - `decode.c`의 `BrotliDecoderCreateInstance`(malloc·free)
  - `shared_dictionary.c`의 `BrotliSharedDictionaryCreateInstance`(malloc)
  - 공개 일회성 API `BrotliDecoderDecompress`. 항상 기본 할당기로 상태를 초기화하므로 adapter는 부르지 않는다.

  llvm-nm으로 보면 CRT 할당 참조(`__imp_malloc`·`__imp_free`)가 `platform`·`decode`·`shared_dictionary` 오브젝트에만 있고
  adapter 오브젝트에는 없다. adapter가 부르는 API에 두 함수를 모두 넘기면 이 분기는 실행되지 않는다. upstream에는 이 경로를 컴파일 단계에서 빼는 설정 지점이 없어(LZ4의
  `LZ4_USER_MEMORY_FUNCTIONS` 같은 것이 없다) 원본을 고치지 않는 한 남는다. 대신 시험이 계수 allocator로 모든 할당이 주입
  할당기를 거치고 해제 뒤 0인지 확인한다.
- 빌드 정의는 없다. `BROTLI_SHARED_COMPILATION`을 정의하지 않으면 `BROTLI_DEC_API`·`BROTLI_COMMON_API`가 비고, 내부
  심볼(`BROTLI_INTERNAL`)은 GNU 계열에서 hidden이다. LZ4·zstd의 visibility 정의를 비운 것과 같은 상태다.
- 외부 심볼은 37개이고 모두 `Brotli`·`kBrotli`·`_kBrotli` 접두사다(llvm-nm). zstd·LZ4·zlib·Compression·Core 오브젝트와 겹치는 이름은
  없다. 같은 이름은 MSVC COMDAT 상수(`??_C@…`·`__xmm@…`)뿐이며 링커가 내용으로 합친다. 외부 헤더는 `brotli/` 하위 경로로만
  include된다.
- 진단 지시문: `platform.h`에 `_Pragma` 5개가 있다. TI·IAR·PGI 컴파일러용 인라인 지시(`BROTLI_INLINE`·`BROTLI_NOINLINE`)이고
  진단 지시문이 아니며 clang·GCC 분기에서는 쓰이지 않는다. manifest `pragmaDirectives`에 기록했다(3.6절).

### 13.3 일회성 계약

- 압축 경로는 모두 Unsupported다: `TryGetCompressionLevelRange`, `CompressionSettings::TryMake`·`TryDefault`,
  `TryGetCompressBound`, 내부 압축 문맥 생성. `CompressionSettings`를 만들 수 없으므로 `Compressor`·`CompressStream`·
  `TryCompress`·비동기 압축·압축 사전은 공개 API로 Brotli에 닿지 않는다.
  - 판정은 `detail::ResolveFormat`(`known`·`compresses`·`streams`) 하나에서 온다. 입구 검사 `CompressFormatError`·
    `StreamFormatError`가 이 값을 쓰고, 비동기 제출과 해제 사전도 같은 함수로 판정한다.
  - 모르는 형식은 InvalidArgument, 아는 형식인데 압축하지 않으면 Unsupported다.
  - 블록 컨테이너는 자기 형식 목록(Lz4·Zstd·Deflate)으로 판정하므로 Brotli는 Lz4Frame·Zlib과 같이 InvalidArgument다.
  - 해제 사전(`DecompressDictionary`)은 Lz4Frame·Zlib과 같이 Unsupported다.
- 해제 경로(Span·Append·`Decompressor`·`DecompressStream`·비동기)는 모두 동작한다. 해제기 상태는 한 stream만 풀 수 있다
  (upstream 계약). 그래서 일회성과 `Decompressor`는 호출마다 상태를 만들고 끝에 반환한다. 문맥이 들고 있는 것은 상태 블록
  하나(48byte)뿐이다. 해제기 상태는 상태 블록 두 개(해제기 5,744byte·shared dictionary 1,408byte, 합 약 7KiB)다.
- stream은 이어 붙일 수 없다. 해제기는 입력을 넘겨 읽지 않으므로(SUCCESS일 때 남은 입력을 돌려준다) 끝난 뒤의 byte는
  1byte도 검출해 InvalidArgument다. 두 번째 stream도 같다. 입력을 모두 넣었는데 더 필요하면 절단(InvalidArgument)이다.
- 출력 상한은 LZ4 frame과 같은 `DecodeOutput`이 맡는다. 쓸 공간이 없으면 1byte 예비 창으로 계속하고, 1byte라도 나오면
  Span은 CapacityExceeded, Append는 출력을 늘린다. 정확히 상한 크기인 결과는 성공한다.
- Append 성장: stream에 원본 크기가 없어 64KiB(상한 이하)를 먼저 잡고, 예비 창에 byte가 나올 때만 두 배씩 늘린다. 실패하면
  길이를 되돌린다.
- 오류 대응:
  - 형식 오류(-1…-17)는 InvalidArgument다.
  - 할당 실패(-21·-22·-25·-26·-27·-30)는 OutOfMemory다.
  - API 계약 오류(-20), 도달할 수 없는 상태(-31), 내장 사전 데이터가 없는 빌드(-19, `BROTLI_EXTERNAL_DICTIONARY_DATA`)는
    DependencyFailed다.
  - 붙인 사전 범위 밖 참조(-18)는 손상이라 InvalidArgument다. 이 빌드는 사전을 붙이지 않는다.
  - 입력·출력 공간이 있는데 진행하지 않으면 DependencyFailed다(무한 반복 방지, upstream에서는 도달하지 않음을 독립 검토가
    확인했다).
- large window(비표준 확장, 첫 byte가 정확히 0x11)는 해제기를 만들기 전에(Append는 출력을 확보하기 전에) Unsupported로
  거부한다. 표지 다음 bit가 1인 0x91은 어느 모드에서도 예약 값이라 해제기가 형식 오류(InvalidArgument)로 보고한다(독립
  검토 지적으로 7bit만 보던 판정을 고쳤다).
- 취소: 입력·출력을 1MiB(`CompressionChunkBytes`)씩 넣어 입력 소비나 출력 생성 1MiB마다 확인한다. 압축률이 아주 높은
  입력(54byte → 64MiB)도 출력 1MiB마다 확인한다.
- 메모리(계수 allocator 측정, Windows x64):

  | 항목 | 값 |
  |---|---|
  | 해제기 상태 | 약 7KiB |
  | ring buffer | 2^k + 542byte. 기본("canny") 방식이라 출력에 필요한 크기(지금 위치 + metablock 길이) 이상의 2의 거듭제곱으로 늘고 window(최대 2^24)를 넘지 않는다. 64byte 출력은 1KiB, 3MiB 출력은 처음부터 4MiB를 쓴다 |
  | Huffman 표 | metablock마다. 최악은 literal·insert&copy·distance 트리 각 256개로 약 2.6MiB(649,216 + 1,107,968 + 919,552 = 2,676,736byte) |
  | context map·block type 표 | context map 최대 17,408byte(metablock마다), block type 표 12,336byte |
  | 최고 사용량 | ring buffer를 늘리는 순간 이전 ring과 새 ring이 함께 있어 약 1.5 × window + 표다. 표준 최대 window에서 약 27MiB(8MiB + 16MiB + 2.6MiB), window를 처음부터 다 쓰는 stream(16MiB metablock)은 약 19MiB다 |

  `BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION`을 켜면 최고값은 window + 표로 고정된다. 대신 WOFF2처럼 작은
  출력에도 window 전체(보통 4MiB, 최대 16MiB)를 바로 잡으므로 기본값을 유지했다. 처음 문서는 늘리는 순간의 최고값을
  빠뜨렸는데, 독립 검토가 여러 metablock stream으로 24MiB를 재현해 바로잡았다.

### 13.4 스트림

- `DecompressStream::TryCreate(Brotli, limits, allocator)`를 쓴다. `CompressStream`은 Unsupported다.
- `maxWindowLog`: 첫 byte의 WBITS(10–24, window 2^WBITS − 16)를 해제기에 넣기 전에 비교한다. 넘으면 CapacityExceeded이고
  ring buffer를 잡지 않는다. 그래서 작업 메모리는 약 1.5 × 2^maxWindowLog + 표로 묶인다. large window 표지는 Unsupported다.
  WBITS는 첫 byte만으로 정해지므로 1byte 조각에서도 같다.
- 경계와 뒤 byte: stream이 끝나면 경계이고, 그 뒤에 오는 byte는 그 호출에서 InvalidArgument다. 같은 호출 안의 남는 byte도
  InvalidArgument다. Deflate·Zlib과 같은 규칙이며 Gzip·Zstd·Lz4Frame처럼 다음 stream으로 읽지 않는다.
- `complete` 판정:
  - 해제기는 입력이 바닥났다고 보고(NEEDS_MORE_INPUT)할 때도 ring buffer에 내보낼 출력을 남길 수 있다. 출력 창이 차서 앞서
    밀어 낸 쓰기가 일부만 끝난 경우다.
  - 그래서 결과 코드가 아니라 `BrotliDecoderHasMoreOutput`으로 남은 출력을 판정한다. 1byte 출력 창으로도 모든 출력이 나온다.
- `Reset`: 해제기 상태를 다시 쓸 수 없어 이전 상태를 반환하고 새로 만든다(이전 ring buffer도 이때 반환한다). 만들지 못하면
  OutOfMemory가 다음 Reset까지 실패 상태로 남는다. 누계 상한·sticky 실패·`TryFinish`는 10절과 같다.

### 13.5 결정

- **large window를 끈다.** 비표준 확장이고 window가 최대 1GiB(2^30)라 메모리 상한을 잃는다. WOFF2와 HTTP
  `Content-Encoding: br`은 표준 window만 쓴다. 필요해지면 `maxWindowLog`로 묶어 켤 수 있다(그때 표지 판정을 확장 WBITS
  검사로 바꾼다).
- **해제 전용을 helper 하나로 표현한다.** `detail::ResolveFormat`이 형식의 `known`·`compresses`·`streams`를 정하고, 입구
  검사(`CompressFormatError`·`StreamFormatError`)와 비동기·사전이 이를 쓴다. 형식 플러그인 등록(UE `ICompressionFormat`,
  후속)을 들이면 `CompressionFormat`을 uint32로 넓히고 `ResolveFormat`을 등록표 조회로 바꾸면 된다. 이번에는 구현하지
  않았다.
- **해제기 상태는 호출마다 만든다.** upstream 상태는 재사용할 수 없다. 상태 생성은 할당 두 번(합 약 7KiB)이라 비용이 작다.

### 13.6 처리량

`CompressionBench --file corpus.bin --brotli corpus.br`로 쟀다. Release, Windows x64, 같은 기기 1회, 단일 스레드다. 코퍼스는
저장소 텍스트(Docs·Core·Engine 소스·C#, 7.7MiB)이고, `.br`은 참조 encoder(Python `brotli` 1.1.0)가 만들었다. 해제 결과가
코퍼스와 같음도 확인했다. Rocks에는 압축기가 없으므로 `--brotli`가 없으면 안내 한 줄만 출력한다.

| 형식 | 수준 | 압축 비율 | 해제 MB/s(일회성) | 해제 MB/s(스트림 64KiB 조각) |
|---|---|---|---|---|
| Brotli | quality 5, window 22 | 0.211 | 753 | 750 |
| Brotli | quality 11, window 22 | 0.179 | 833 | 809 |
| Brotli | quality 11, window 24 | 0.178 | 814 | 825 |
| Zstd(같은 코퍼스) | 3 / 19 | 0.245 / 0.183 | 1433 / 1580 | — |
| Deflate(같은 코퍼스) | 6 | 0.249 | 556 | — |

측정 구간에는 해제만 넣고 결과 비교는 구간 밖에서 한다. quality 11은 zstd 19보다 조금 작고, 해제는 zstd의 약 53%이며
Deflate보다 빠르다.

### 13.7 시험

`Tests/BrotliTests.cpp` 15개(Windows x64 Debug 약 1.0초, ASan 약 2.5초, Release 약 0.4초). 벡터는 참조 encoder(Python 3 + `pip install
brotli==1.1.0`, google/brotli 1.1.0의 공식 binding)가 만든 13개(합 4,107byte, 약 4.0KiB)와 손으로 만든 3개다.

- 참조 벡터: quality 1·5·9·11, window 10·16·18·20·22·24, `MODE_FONT`, 빈 입력, 비압축 metablock(난수 512byte), 고압축 입력
  (3MiB, 64MiB 폭탄)
- 손으로 만든 입력: large window 표지(0x11), 예약 window(0x91), 패딩 bit 오류
- 생성 방법(같은 byte가 다시 나온다)은 시험 파일 머리 주석에 있다. 입력 생성기는 시험의 `Fill`·`MakeSparse`를 그대로 옮겼고,
  생성 뒤 `brotli.decompress`로 원본과 같음을 확인했다. 주석에 원본 CRC-32와 WBITS를 적었다.

시험 목록:

- 해제 전용: 압축 경로 Unsupported, 모르는 형식 InvalidArgument, `ResolveFormat`·입구 검사, 해제 문맥·스트림 생성과 한계 검사,
  해제 사전 Unsupported, 블록 컨테이너 InvalidArgument, 빈 입력·겹침
- 왕복: 참조 벡터 11개(폭탄·ring 성장 벡터 제외)를 Span·Append(접두 보존)·`Decompressor` 재사용·스트림(조각 1·7·4096·전체)으로
  풀어 비교, 경계 뒤 빈 입력
- 역압: 출력 창 1·7·4096byte × 입력 조각 1·전체
- 상한: 정확한 상한 성공, 1byte 부족 CapacityExceeded(Span·Append 길이 복원·스트림 sticky·큰 창 TryProcess), 상한 0인 빈 stream
- 뒤 byte·연결: 쓰레기 1byte, 같은 stream 두 개(일회성·스트림 전체·조각 1·7: stream 끝과 뒤 byte가 한 조각에 오는 경우
  포함), 경계 뒤 byte, Reset 복구
- 절단: 모든 길이(문맥·Append·스트림 경계)
- bit flip: 모든 bit(Span·Append), byte마다 1bit(스트림). 허용 결과 안이고 범위 밖 쓰기가 없다(ASan)
- 폭탄: 54byte → 64MiB를 상한 1MiB로(일회성·스트림, 최고 사용량 < window + 2 × 상한 + 표), `maxWindowLog` 22로 첫 byte에서
  거부(< 64KiB), 64KiB 창으로 끝까지 해제
- window: 모든 벡터의 `maxWindowLog` = WBITS 성공·WBITS − 1 CapacityExceeded, large window 표지 Unsupported(일회성 Span·Append는
  할당 없이), 첫 byte 256개 전부의 분류, 예약 값 0x91 InvalidArgument
- Reset·sticky: 손상 뒤 sticky, 진행 중·끝난 뒤 Reset
- OOM: 할당 시도 위치마다(Append 성장, Span, 문맥, 스트림 생성·쓰기, Reset의 재생성: 해제기 상태·shared dictionary 상태). 누수 0, 길이 복원, sticky·Reset 복구
- 할당 경로: 계수 allocator. 일회성 반환 0, ring buffer가 주입 할당기를 거침(최고 ≥ 출력 3MiB, < 8MiB), 작은 출력의 작은 ring,
  ring을 늘리는 순간의 최고값(1.25 × window < 최고 < 1.5 × window + 64KiB, 이전 ring과 새 ring이 함께 있음), 문맥의 무보유, 스트림 Reset의 반환
- 취소 probe: 1MiB 단위 확인과 Append 길이 복원(1KiB → 3MiB)
- 비동기 = 동기: Append·Span, 의존 연결, large window·뒤 byte 실패 결과, 제출 시점 거부
- WOFF2식 사용: 헤더가 알려 준 크기로 Span 해제, 더 큰 선언(결과 크기 불일치), 더 작은 선언(CapacityExceeded)

### 13.8 후속

- macOS ARM64·Android 실기기 실행.
- WOFF2 글꼴 importer(표 변환 복원)는 글꼴 모듈의 몫이다.
- Brotli 압축(`c/enc`)과 사용자 사전·large window는 수요가 생기면 다룬다.

## 14. 형식 플러그인 등록 (7차, 교차검증 반영)

UE 5.6 `ICompressionFormat` 대응이다. UE는 `IModularFeatures`("CompressionFormat")에 형식을 등록하고
`FCompression::GetCompressionFormat(FName)`이 내장 형식(Zlib·Gzip·LZ4·Oodle) 다음으로 찾는다. 형식은 `FName`으로 식별하고
`GetVersion`·`GetDDCKeySuffix`를 빌드 캐시 키에 넣는다. Rocks도 저장소에 넣지 않는 외부 코덱(라이선스상 프로젝트가 따로
가져오는 코덱 등)을 프로젝트·플러그인이 등록해 내장 형식과 같은 API로 쓰게 한다. 헤더는 `Compression/CompressionCodec.h`,
구현은 `Private/CompressionCodec.cpp`다. 설계 초안은 교차검증(값 인코딩·수명·비동기 연동·거부 표면)을 거쳤다.

### 14.1 식별: 형식 값과 이름

- `CompressionFormat`을 `uint32`로 넓혔다. 0–255는 내장 형식이고 256(0x100) 이상이 사용자 등록 형식이다. 등록할 때마다
  다음 값을 발급하고 해제한 값은 다시 쓰지 않는다(오래된 값이 다른 코덱으로 해석되지 않는다). 해제됐거나 해제가 시작된
  값은 모르는 형식 값과 같이 InvalidArgument다. 0xFFFFFFFF까지 발급한 뒤의 등록은 CapacityExceeded다.
- 별도 handle 타입 대신 같은 enum을 쓴다. 모든 API가 `CompressionFormat`을 받으므로 `CompressionSettings`·`Compressor`·
  비동기 제출이 그대로 연동되고, 값 재사용 금지로 handle의 세대 검사와 같은 효과를 얻는다.
- 이름 문법: 소문자 ASCII `[a-z0-9_-]`로 된 비지 않은 조각을 `.`로 이은 1–32byte다. 내장 이름(`lz4`·`zstd`·`deflate`·
  `zlib`·`gzip`·`lz4frame`·`brotli`)은 조각이 하나이고, 사용자 등록 이름은 둘 이상이어야 해서(예 `oodle.kraken`) 절대
  겹치지 않는다.
- 사용자 등록 형식의 값은 등록 순서에 따라 실행마다 다르다. 파일·빌드 캐시 키에는 `TryGetCompressionFormatInfo`의
  이름과 revision(내장 형식은 `CompressionOutputRevision`, 사용자 등록 형식은 등록 version)을 수준·사전 Digest와 함께
  쓴다. 설정 파일의 이름은 `TryFindCompressionFormat`으로 값으로 바꾼다(문법 위반 InvalidArgument, 없거나 해제 중이면
  NotFound).

### 14.2 코덱 계약 (`CompressionCodec`)

```cpp
class CompressionCodec {
public:
    virtual ~CompressionCodec() = default;
    virtual Result<usize> TryGetCompressBound(usize sourceBytes) const noexcept = 0;
    virtual Result<usize> TryCompress(int32 level, Span<const uint8> source, Span<uint8> destination,
                                      Allocator& scratch) const noexcept = 0;
    virtual Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination,
                                        Allocator& scratch) const noexcept = 0;
};

struct CompressionCodecDescription {
    StringView name;              // 복사한다
    uint32 version = 0;           // 출력 byte가 바뀔 때마다 올린다
    CompressionLevelRange levels; // minimum ≤ defaultLevel ≤ maximum
};
inline constexpr uint32 MaxCompressionCodecs = 16;
Result<CompressionFormat> TryRegisterCompressionCodec(const CompressionCodecDescription&, const CompressionCodec&) noexcept;
Result<void> TryUnregisterCompressionCodec(CompressionFormat format) noexcept;
```

- 코덱은 상태가 없어야 하며 여러 스레드가 동시에 부른다. 작업 메모리는 호출마다 scratch에서 받아 반환 전에 돌려준다.
  코덱 안에서 이 모듈(다른 형식 압축 등)을 다시 불러도 된다. 출력 결정성은 코덱 책임이다.
- 모듈이 호출 전에 보장하는 것: source·destination이 겹치지 않음, 빈 Span도 `Data()`가 nullptr가 아님, 수준이 등록 범위
  안, 해제 입력이 비지 않음.
- 반환 계약과 위반 처리(위반은 DependencyFailed로 바꾼다. 코덱이 외부 라이브러리를 감싸므로 내장 adapter의 "외부 코덱이
  계약 밖의 실패를 보고"와 같은 분류다):

| 함수 | 허용 오류 | 위반 |
|---|---|---|
| `TryGetCompressBound` | InvalidArgument·Overflow | 그 밖의 오류 |
| `TryCompress` | InvalidArgument·CapacityExceeded·OutOfMemory·Overflow·DependencyFailed | 반환값 > destination, destination ≥ bound인데 CapacityExceeded, 그 밖의 오류(Unsupported·Cancelled 포함) |
| `TryDecompress` | 위 다섯 가지 + Unsupported | 반환값 > destination, 그 밖의 오류(Cancelled 포함) |

- 결과가 destination보다 크면 해제는 반드시 CapacityExceeded여야 한다. 크기를 모르는 해제의 재시도가 이 오류에 기댄다.
- 가상 Append·스트림·사전 인터페이스는 두지 않았다. 코덱 계약을 UE `Compress`·`Uncompress`·`GetCompressedBufferSize`와
  같은 최소 집합으로 유지하고, 크기를 모르는 해제는 모듈이 일반화한다(14.4절).

### 14.3 등록 표와 사용권

- 표는 고정 16칸이며 할당하지 않는다. 표와 mutex는 첫 호출에서 placement-new로 만들고 해제하지 않는다(프로세스 종료 중
  다른 스레드가 압축해도 소멸된 mutex를 쓰지 않고, 정적 초기화 순서와 무관하다. 11.6절 학습 mutex와 같은 방식). 캐시가
  아니라 설정이며, 내장 형식 경로는 표를 읽지 않는다. 등록·해제·조회 함수는 어느 스레드에서나(코덱 안에서도) 부를 수 있다.
- 사용권(lease): 칸마다 사용 수와 해제 중 표시를 둔다. 일회성 호출은 호출 동안, `Compressor`·`Decompressor`는 문맥 수명
  동안 쥔다. 비동기 작업은 실행이 끝날 때, 실행되지 않으면 capture가 정리될 때 놓는다(늦어도 terminal 게시 전, 5절).
  사용권을 얻을 때와 놓을 때만 표를 잠그고, 코덱 호출 중에는 잠그지 않는다(재진입 가능).
- `TryUnregisterCompressionCodec`:
  - 처음 부른 순간 해제 중으로 표시해 그 값의 새 사용(일회성 호출·문맥 생성·비동기 제출·수준 범위·bound·형식 정보)을
    InvalidArgument로, 이름 조회를 NotFound로 막는다. 해제 중인 이름은 칸을 차지하므로 같은 이름 등록은 AlreadyExists다.
  - 사용권이 남아 있으면 Busy다. 남은 사용(진행 중인 호출, 살아 있는 문맥, 대기 중이거나 실행 중인 비동기 작업)은 끝까지
    동작하며, 끝난 뒤 다시 부르면 칸을 비우고 성공한다. 성공한 뒤에만 codec을 파괴하거나 모듈을 언로드한다.
  - 코덱이 자기 형식을 해제하면 자기 호출이 사용권을 쥐고 있어 Busy다.
  - 내장 형식·등록되지 않은 값은 InvalidArgument다.
- UE는 사용 중인 형식의 등록 해제를 막지 않는다(modular feature 제거 뒤의 사용은 호출자 책임). Rocks는 사용권과 Busy로
  언로드 경합을 오류로 드러낸다.

### 14.4 API 연동

| API | 사용자 등록 형식 |
|---|---|
| `TryGetCompressionLevelRange`·`CompressionSettings` | 등록한 범위 |
| `TryGetCompressBound` | 코덱 bound(사용권을 쥐고 부른다) |
| `TryCompress`·`TryDecompress` | 상태 블록을 스택에 둔다(할당 없음) |
| `TryCompressAppend` | 코덱 bound만큼 늘려 압축한 뒤 줄인다. 실패하면 길이를 되돌린다 |
| `TryDecompressAppend` | 64KiB와 입력 × 4 중 큰 값에서 시작하고(입력 × 4가 상한을 넘으면 상한에서 시작), 코덱이 CapacityExceeded면 두 배로 늘려 처음부터 다시 해제한다. 상한까지 늘려도 부족하면 CapacityExceeded, 늘리다 할당이 실패하면 OutOfMemory다. 손상 입력도 상한까지 메모리를 쓸 수 있어 상한은 현실적인 값으로 준다. Lz4 raw block도 같은 첫 크기 규칙이다 |
| `Compressor`·`Decompressor` | 문맥 상태가 사용권을 가진다(코덱 상태는 없다) |
| 비동기 제출 | 제출할 때 사용권을 얻는다(5절) |
| `CompressStream`·`DecompressStream`·`CompressDictionary`·`DecompressDictionary` | Unsupported |
| `CompressedBlocksSettings`·`CompressedBlocksReadContext::TryCreate(format, …)` | InvalidArgument(codec ID가 영속 값) |
| `TryGetCompressionFormatInfo`·`TryFindCompressionFormat` | 등록 이름·version, `plugin = true` |

- 취소: 코덱 호출 하나는 끊지 않는다(Lz4 raw block과 같다). 비동기 작업은 코덱 호출 전마다(Append는 재시도 사이 포함)
  취소를 확인한다.
- 형식 판정: 내장 형식은 `detail::ResolveFormat`(known·compresses·streams·plugin) 하나에서 나온다. 사용자 등록 형식은
  사용권 획득(`CodecLease::TryAcquire`)이나 수준 범위 조회가 판정을 겸하고(수준·bound·문맥·일회성·비동기 압축 제출),
  `ResolveFormat`은 스트림·사전·비동기 해제 제출 입구에서 그 값을 판정한다. 사용자 등록 형식은 압축하고 스트림은 하지
  않는 형식이다. 설정을 만든 뒤 해제가 시작된 형식은 어느 입구에서든 모르는 값(InvalidArgument)이다.
- 사전 학습(`CompressionTraining`)은 zstd 전용이라 영향이 없다. `CompressionBench`는 형식 이름을
  `TryGetCompressionFormatInfo`로 출력한다.

### 14.5 시험

`CompressionCodecTests.cpp` 13개. 가짜 코덱은 XOR(수준 byte + XOR, scratch 사용), RLE(해제 호출 수 계수), 계약 위반 코덱,
진입을 알리고 멈추는 코덱, 재진입 코덱이다. 등록 표가 프로세스 전역이고 시험은 이름순으로 한 프로세스에서 실행되므로, 각
시험은 `ScopedCodec`으로 등록해 일찍 끝나도 해제를 확인한다.

- 등록 표: 이름 문법 위반(빈 이름·조각 하나·대문자·앞뒤·연속 `.`·공백·기호·33byte), 수준 범위 역전, 중복 이름, 17번째
  등록, 해제 중인 칸도 용량에 포함, 32byte 이름, 값의 단조 증가와 재사용 금지, 내장·모르는 값 해제
- 형식 정보: 내장 이름 golden과 이름 ↔ 값 왕복, 해제 전용 표시, 사용자 등록 형식의 이름·version, 이름 문법, 해제 뒤 조회
- 공개 API 왕복: 일회성 Span·Append(기존 byte 보존), `Compressor`·`Decompressor`의 Span·Append(같은 byte), bound·overflow,
  수준 검증, 크기를 아는 해제의 정확한 크기·1byte 부족·빈 입력·겹침
- 두 배 재시도 경계: 64KiB 정확, 64KiB + 1, 1MiB(호출 5회), 상한 1 부족, 첫 크기가 상한, 입력 × 4 시작, 입력 × 4가
  상한을 넘으면 상한 시작, 손상 시 재시도 없음, 실패 시 길이 복원, 재시도 사이 취소(호출 0·1·2회에서 Cancelled)
- 계약 위반: 반환 크기 초과, bound 이상에서 CapacityExceeded(bound 미만은 그대로), 허용 목록 밖 오류(압축 Unsupported·
  Cancelled·NotFound·Busy), bound 오류, Append 길이 복원
- 거부 표면: 스트림·사전 Unsupported, 블록 설정·읽기 문맥 InvalidArgument, 해제 뒤(이미 만든 설정 포함) InvalidArgument
- 사용권(문맥): 살아 있는 문맥이면 Busy, 해제 중 새 사용(설정 생성·일회성 압축·해제·문맥 생성·bound·수준·정보·이름)
  거부·기존 문맥은 동작, 마지막 문맥을 놓은 뒤 성공
- 사용권(실행 중): 비동기 작업과 다른 스레드의 일회성 호출이 코덱 안에 있는 동안 Busy·새 제출 InvalidArgument, 끝난 뒤 성공
- 비동기: 대기 중 작업이 있으면 Busy·새 제출 InvalidArgument, 대기 작업은 끝까지 실행(동기와 같은 byte, Append·Span 해제),
  `TryWaitResult` 직후 해제 성공. 실행되지 않은 작업(body 할당 실패, 실행 전 취소, 의존 실패)도 사용권을 놓음
- 재진입: 코덱 안에서 Zstd 왕복·다른 사용자 형식 압축, 자기 형식 해제 Busy
- OOM: 문맥 상태·코덱 scratch·output 성장의 모든 할당 시도 실패에서 OutOfMemory·길이 복원·사용권 누수 없음
- 동시성: worker 4개가 일회성 왕복을 반복하는 동안 주 스레드가 다른 형식의 등록·해제를 200회 되풀이한다. 코덱은 라운드마다
  heap에 만들고 해제가 성공하면 파괴하므로 사용권이 틀리면 ASan이 해제 뒤 사용을 잡는다. 해제 중 사용은
  InvalidArgument·NotFound로만 실패하고, 성공과 거부가 모두 일어났는지(실제로 겹쳤는지) 확인한다

### 14.6 후속

- 사용자 등록 형식의 스트림·사전(코덱 인터페이스 확장)은 수요가 생기면 다룬다.
- 블록 컨테이너에 사용자 형식을 쓰려면 영속 codec ID 대신 형식 이름을 기록하는 컨테이너 version이 필요하다.
