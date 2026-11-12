# 압축 헬퍼 — 코덱 선정·빌드 통합·동기/비동기 API

기준일: 2026-11-12. Godot `Compression`(core/io/compression.h)과 UE 5.6 `FCompression`을 참고해, 엔진·도구·게임이 함께
쓰는 압축 헬퍼를 Core 계층에 둔다. 요구 조건은 **성능**, **라이선스 문제 없음**, **포팅 문제 없음**이다. 이 문서는 네 관점
(라이선스·포팅, API 계약·악성 입력, 빌드·정책 통합, 비동기)의 병렬 교차검증을 거친 설계다. 구현·실행 검증 결과는
[구현 상태](../Implementation/FoundationStatus.ko.md)와 별도 검증 기록을 따른다.

## 1. 범위

| 이번 범위 | 후속 |
|---|---|
| 형식을 고르는 일회성 압축·해제, 최악 압축 크기, 크기를 모르는 해제(상한 필수, `Array` 뒤에 붙임), 재사용 문맥(`Compressor`·`Decompressor`), worker 비동기 압축·해제(의존 작업 연결·협력적 취소), 모든 할당의 Rocks `Allocator` 경유, 결정적 출력 | 블록 단위 seek 가능 컨테이너(Godot `FileAccessCompressed`·UE `FCompressedBuffer` 대응)와 큰 버퍼의 블록 병렬 압축, gzip·zstd 스트리밍 객체, zstd 사전, Brotli 해제(WOFF2), 형식 플러그인 등록(UE `ICompressionFormat`), AsyncFile 완료 → 해제 작업 연결(외부 완료 이벤트 handle 필요), zstd ultra 수준(20–22) |

## 2. 코덱 선정

| 형식 | 라이브러리 | 라이선스(SPDX) | 선정 이유 |
|---|---|---|---|
| `Lz4`(raw block) | LZ4 1.11.1(2026-11-10), `lib/`의 `lz4.[ch]`·`lz4hc.[ch]`만 | BSD-2-Clause. 원문은 `lib/LICENSE`다. 저장소 루트 `LICENSE`는 "lib은 BSD-2, 나머지는 GPL-2.0-or-later" 안내문이며 CLI(`programs/`)는 들이지 않는다 | 해제 최고속. 1.11은 HC 확장 상태(`LZ4_STREAMHC_EXTSIZE`)로 수준 10–12도 할당 없이 동작하고, 1.10.0이 Android(non-Apple aarch64 clang)에서 껐던 빠른 해제 루프를 켠다. 1.11.0의 회귀는 폐기 API `LZ4_resetStreamHC()`에 국한되며 Rocks는 쓰지 않는다 |
| `Zstd`(frame, RFC 8878) | zstd 1.5.7(2025-02-19, 최신 안정) | `BSD-3-Clause OR GPL-2.0-only` 중 **BSD-3-Clause 선택**. 부분집합 전 파일(xxhash 포함)이 이중 라이선스이며 특허 조항은 없다 | 압축률·속도 균형, `ZSTD_customMem`으로 할당기 주입, 원본 크기·체크섬 기록. upstream C 라이브러리의 알려진 CVE는 없다(검색된 CVE는 zstd-jni 것) |
| `Deflate`(RFC 1951)·`Zlib`(RFC 1950)·`Gzip`(RFC 1952) | zlib 1.4(2026-11-04) | Zlib | 호환 형식(PNG·gzip·zip). `zalloc`·`zfree`·`opaque`로 할당기 주입, 스트림 API로 4GiB 이상과 취소를 처리. 1.4는 최신 유지와 UB·Android·Windows 이식성 수정이 근거다(보안 수정 대부분은 Rocks가 쓰지 않는 gz·inflateSync·Prime·inflateBack API) |

제외한 후보: Oodle(독점 라이선스), FastLZ(LZ4가 속도·압축률 모두 우위), libdeflate(MIT·Deflate 최고속이지만 스트리밍이
없고 할당 함수에 opaque 인자가 없다. Deflate가 hot path가 되면 adapter 뒤에서 교체 후보), zlib-ng(빠르지만 플랫폼별 SIMD
구성 생성이 필요해 포팅 부담), LZMA·Brotli(현재 용도 없음).

**고지 의무.** BSD-2·BSD-3은 바이너리 배포 시 저작권 고지·조건·면책 문구를 문서나 함께 배포하는 자료에 재현해야 한다.
Zlib 라이선스는 바이너리 고지 의무가 없고 수정본은 수정본임을 밝혀야 한다. Rocks는 원본을 수정하지 않는다. 코덱은 첫
버전부터 엔진 바이너리에 들어가므로 **외부 배포(설치 SDK·게임 패키지·APK) 전에 고지 묶음이 반드시 포함되도록 배포 경로에
gate를 둔다.** 설치 SDK는 각 라이브러리의 LICENSE를 함께 복사한다.

## 3. 저장소와 빌드

### 3.1 배치

외부 원본은 Core가 쓰므로 Core 계층에 둔다(Core는 Engine을 참조하지 않는다). UE `Engine/Source/ThirdParty/<lib>/<version>/`과
같은 방식이다.

```text
Core/Source/ThirdParty/
  Lz4/   Lz4.Build.cs   ThirdParty.json   Lz4Unity.c   lz4-1.11.1/lib/{LICENSE, lz4.c, lz4.h, lz4hc.c, lz4hc.h}
  Zstd/  Zstd.Build.cs  ThirdParty.json   zstd-1.5.7/{LICENSE, lib/zstd.h, lib/zstd_errors.h, lib/common/…, lib/compress/…, lib/decompress/…}
  Zlib/  Zlib.Build.cs  ThirdParty.json   zlib-1.4/{LICENSE, adler32.c, crc32.[ch], deflate.[ch], inffast.[ch], inffixed.h,
                                                    inflate.[ch], inftrees.[ch], trees.[ch], zconf.h, zlib.h, zutil.[ch]}
Core/Source/Runtime/Compression/   Compression.Build.cs  Public/Compression/  Private/  Tests/
```

- 버전 디렉터리(`<lib>-<version>/`)는 릴리스 tarball의 파일을 **byte 그대로** 둔다. `.gitattributes`는 이 디렉터리를
  `-text`로, `.editorconfig`는 trim·final newline 규칙의 예외로 둔다(줄끝·공백 변환 방지).
- 컴파일하지 않는 파일은 넣지 않는다. zstd는 `legacy/ deprecated/ dictBuilder/`, `zstdmt_compress.c pool.c threading.c
  debug.c huf_decompress_amd64.S`를 뺀 22개 번역 단위, zlib은 `gz*.c compress.c uncompr.c infback.c`를 뺀 8개, LZ4는
  `lz4frame* lz4file* xxhash*`를 뺀다.
- LZ4는 Rocks 글루 `Lz4Unity.c`(`#include "lz4.c"` 다음 `#include "lz4hc.c"`) 한 번역 단위로 빌드한다. upstream이 지원하는
  방식(`LZ4_SRC_INCLUDED`)이며 이렇게 하면 `lz4hc.c`의 `-Wunused-function` 억제 분기가 비활성이다.

### 3.2 manifest(`ThirdParty.json`) — 단일 출처

라이브러리마다 다음을 기록한다. 모듈 규칙과 정책 검사가 같은 파일을 읽는다(규칙에 목록을 중복하지 않는다).

| 필드 | 뜻 |
|---|---|
| `name`·`version`·`upstreamDirectory` | 이름·고정 버전·원본 디렉터리(라이브러리 폴더 기준) |
| `source` | 릴리스 URL, tarball SHA-256, 서명 검증 방법(zlib `.asc`, zstd 공식 `.sha256`) |
| `license` | 선택한 SPDX, upstream 표현식, 원문 파일 경로, 바이너리 고지 필요 여부 |
| `files` | 원본 디렉터리 안 모든 파일의 상대 경로 → SHA-256(byte 기준) |
| `translationUnits` | 컴파일할 파일(라이브러리 폴더 기준. 원본 파일 또는 Rocks 글루) |
| `includeDirectories` | adapter가 private으로 쓰는 include 경로 |
| `defines`·`platformDefines` | 라이브러리 빌드 정의(3.4) |
| `consumerDefines` | adapter가 헤더를 include할 때 같아야 하는 정의(`Z_PREFIX` 등) |
| `adapterHeaders` | 엔진 소유 번역 단위가 include하는 외부 헤더 |
| `pragmaDirectives` | 원본의 컴파일러 지시문 전체: `#pragma`(`once` 제외, MSVC 번호 형식 포함)·`_Pragma`·`__pragma`·경고/deprecation 억제 define. 줄 이음을 합친 논리 줄 기준(파일·시작 줄·원문). 3.6 참고 |
| `notes` | 실험 API 고정 사유, 설정 매크로 근거 등 |

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
| LZ4 | `LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION` | 할당 API를 컴파일 단계에서 제거(남은 할당 경로는 컴파일 오류). adapter는 상태를 직접 할당한다 |
| LZ4 | `LZ4LIB_VISIBILITY=` | ELF·Mach-O에서 `visibility("default")` 강제 해제(Android `.so` 심볼 노출 방지) |
| zstd | `ZSTD_DISABLE_ASM=1` | `.S` 없이 모든 플랫폼 같은 C 경로. 잃는 것은 Android x86_64의 asm Huffman 해제뿐 |
| zstd | `ZSTD_TRACE=0`, 멀티스레드·legacy 미정의 | ELF weak symbol trace 비활성, 단일 스레드, 구형 형식 제외 |
| zstd | `ZSTDLIB_VISIBLE=`, `ZSTDERRORLIB_VISIBLE=` | 심볼 노출 해제(위와 같음) |
| zstd | `MEM_STATIC=static __inline __attribute__((unused))` | clang-cl은 `__GNUC__`가 없어 zstd가 GNU 계열에서 기본으로 붙이는 `unused` 속성을 빼므로, assert 전용 `HUF_isSorted`·`ZSTD_isAligned`가 `-Wunused-function` 오류가 된다. zstd의 `#ifndef MEM_STATIC` 가드(커널 빌드용, README 설정 항목은 아님)로 GNU 계열 정의를 그대로 재현한다. 경고 정책 "외부 원본 내부 진단 지시문" 4항으로 승인 |
| zstd(Windows) | `DYNAMIC_BMI2=0` | clang-cl에서는 `TARGET_ATTRIBUTE`가 비어 BMI2 사본이 이득 없이 중복된다(README 문서화 설정). 해제 속도 측정으로 확인 |
| zlib | `Z_SOLO` | 기본 할당기·gz 파일 IO 제거(할당기 주입 강제, `_CRT_SECURE_NO_WARNINGS`를 정의하는 `gzguts.h` 미포함). `memcpy`가 내부 루프로 바뀌지만 `-O2`에서 벡터화 복사로 컴파일됨을 확인 |
| zlib | `Z_U4=__UINT32_TYPE__`, `Z_U8=__UINT64_TYPE__` | `Z_SOLO`가 끄는 braid CRC-32·ARMv8 하드웨어 CRC를 되살린다(Gzip 성능) |
| zlib | `Z_PREFIX` | 심볼에 `z_` 접두사(Android·macOS 시스템 libz와 충돌 방지) |
| zlib(Windows 외) | `HAVE_HIDDEN` | 내부 심볼 hidden |

`Z_SOLO`·`Z_PREFIX`·`Z_U4`·`Z_U8`, zstd의 visibility 정의와 `ZSTD_STATIC_LINKING_ONLY`, LZ4의 `LZ4_HC_STATIC_LINKING_ONLY`·
`LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION`·`LZ4LIB_VISIBILITY=`는 adapter에도 같은 값으로 준다(`consumerDefines`).
같은 이름이 라이브러리 정의에도 있으면 값이 같아야 한다(manifest 리더가 확인). `LZ4_STATIC_LINKING_ONLY`는 `lz4hc.h`가
`LZ4_HC_STATIC_LINKING_ONLY`일 때 스스로 정의하므로 명령줄로 주지 않는다(값이 다르면 재정의 경고). LZ4 HC 확장 상태 API는
1.11의 실험 API이므로 manifest `notes`에 고정 사유를 남긴다.

### 3.5 정책 검사

- 기존 lexical 검사(서식, 경고 억제, 플랫폼 경계, 강제 인라인 금지)는 **manifest의 `upstreamDirectory`와 정확히 같은
  디렉터리만** 제외한다. 이름이 비슷한 디렉터리는 계속 검사한다. Rocks 글루(`*.Build.cs`, `Lz4Unity.c`)는 검사 대상이며
  검사 대상 확장자에 `.c`를 더한다.
- 새 `third-party` 검사: manifest 스키마 엄격 검사(모르는·중복 필드 거부), 실제 파일 집합과 SHA-256(byte 기준) 일치,
  링크 파일 거부, LICENSE 존재, `translationUnits` ⊆ 원본 파일 ∪ 글루, 컴파일러 지시문 목록(`pragmaDirectives`)의 정확한
  일치.
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
`huf_compress.c`(4127), LZ4 `lz4.c`(4127·6237·6239·6240·6326, push/6385/pop), zlib `deflate.c`(push/4127/pop),
`zstd.h`(clang push/`-Wzero-as-null-pointer-constant`/pop). **clang-cl은 일부 MSVC 번호를 clang 진단에 매핑해 실제로
따른다**(4100 → `-Wunused-parameter` 확인). 그래서 원본의 진단 지시문을 모두 지운 사본을 같은 프로필로 컴파일했다. 결과는
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
`Zstd`·`Zlib`이다. Core 모듈에 외부 의존이 생기지 않고, 비동기 API가 Task를 쓸 수 있다. 정적 링크이므로 동기 API만 쓰는
모듈(예: AssetTools PNG)은 Task의 object를 링크하지 않는다.

```cpp
namespace rocks {

// 프로세스 안 식별용이다. 영속 포맷에 기록할 때는 상위 포맷이 별도 ID를 정한다.
enum class CompressionFormat : uint8 { Lz4, Zstd, Deflate, Zlib, Gzip };

struct CompressionLevelRange {
    int32 minimum;
    int32 maximum;
    int32 defaultLevel;
};

// Lz4 {1, 12, 1}: 1 = fast(acceleration 1), 2 = lz4mid, 3–9 = HC, 10–12 = HC optimal parser.
// Zstd {1, 19, 3}. Deflate·Zlib·Gzip {0, 9, 6}(0 = stored). 모르는 형식은 InvalidArgument.
Result<CompressionLevelRange> TryGetCompressionLevelRange(CompressionFormat format) noexcept;

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
  재사용 문맥으로 성능을 얻는다.
- **결정성**: 같은 라이브러리 버전·설정·입력이면 64bit little-endian 대상(Windows x64·macOS ARM64·Android arm64/x86_64)
  에서 같은 byte다. LZ4는 32bit 대상에서 hash·table 형식이 달라 결정성 범위 밖이다.
  - zlib: windowBits 15·memLevel 8·`Z_DEFAULT_STRATEGY`로 고정한다. Gzip 헤더는 매 reset 뒤 `deflateSetHeader`로
    mtime 0·OS 255·이름·주석·extra·FHCRC 없음을 다시 적용한다(zlib 1.4 `deflateResetKeep`이 헤더를 지우고, 기본 OS 코드는
    플랫폼마다 다르다).
  - zstd: checksum과 content size를 켜고 dictID는 쓰지 않는다.
  - 동기·비동기는 같은 내부 구현과 같은 chunk 상수를 쓴다.
  - 테스트가 고정 입력의 압축 결과 CRC-32를 golden으로 고정한다. 버전을 올릴 때는 의도적으로 갱신한다.
- **무결성**: Zstd는 content checksum(XXH64 하위 32bit), Zlib은 Adler-32, Gzip은 CRC-32·ISIZE를 검사한다. Lz4 raw block과
  Deflate raw는 무결성 검사가 없어 손상이 "성공한 다른 데이터"로 보일 수 있다. 체크섬 형식도 의미 없는 bit(zstd 프레임
  헤더의 미사용 bit, deflate 마지막 byte의 padding, FHCRC 없는 gzip의 MTIME·XFL·OS)는 바꿔도 같은 결과로 성공할 수 있다.
  따라서 계약은 "손상은 실패하거나, 성공하면 원본과 같은 출력"이다.

### 4.2 형식별 계약과 오류 대응

| 상황 | 오류 |
|---|---|
| destination 또는 `maxOutputBytes` 부족 | CapacityExceeded |
| 손상·절단·체크섬 불일치·스트림 뒤 남는 byte(Lz4 제외)·빈 해제 입력·범위 밖 수준·겹침·Lz4 입력 한계 초과 | InvalidArgument |
| zlib 프리셋 사전(FDICT, `Z_NEED_DICT`)·사전이 필요한 zstd frame(`dictionary_wrong`) | Unsupported |
| 할당 실패(`Z_MEM_ERROR`·`memory_allocation` 포함) | OutOfMemory |
| 크기 산술 overflow, `deflateBound_z`의 `(z_size_t)-1` | Overflow |
| 외부 코덱이 계약 밖의 실패를 보고(예: dst ≥ bound인데 LZ4 압축이 0 반환, `Z_STREAM_ERROR`) | DependencyFailed |

- **Lz4**
  - 압축: fast(수준 1)는 `LZ4_initStream` 후 `LZ4_compress_fast_extState_fastReset`을 쓴다. HC(2–12)는
    `LZ4_STREAMHC_EXTSIZE` 버퍼를 `LZ4_initStreamHC`로 한 번 초기화한 뒤 `LZ4_compress_HC_extStateHC_fastReset`을 쓴다(상태
    8byte 정렬). 0 반환은 dst < bound면 CapacityExceeded, 아니면 DependencyFailed다.
  - 해제: `LZ4_decompress_safe`의 -1과 모르는 음수는 InvalidArgument다. -2("출력 부족 또는 손상")는 용량이 LZ4 최대 확장비
    (255 × 입력 + 상수)에 이르렀으면 InvalidArgument, 아니면 CapacityExceeded다. raw block에는 끝 표지가 없으므로 "남는
    byte" 검사는 하지 않는다.
  - int 경계: source가 `Lz4MaxInputBytes`를 넘으면 InvalidArgument, destination 용량은 `INT_MAX`로 자른다.
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
- **수준과 메모리**: zstd 수준 19의 압축 문맥은 큰 입력에서 수십 MiB를 쓰고(ultra 20–22는 수백 MiB라 제외), LZ4 HC 확장
  상태는 약 320KiB, zlib deflate는 약 256KiB, inflate는 약 40KiB, zstd 해제 문맥은 약 160KiB다. 정확한 값은 테스트의 계수
  allocator로 기록한다.

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
  1. output을 읽지 않는 동기 검증(Lz4 입력 한계·빈 해제 입력·모르는 형식·Span 변형의 source·destination 겹침).
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
  - 취소 가능 경로: zlib 계열 전체, zstd 압축·해제.
  - 끊지 않는 경로는 LZ4 압축·해제다. LZ4 fast·해제는 수 초 이내지만 HC 12는 약 10–20MB/s여서 2GiB 가까운 입력이면
    100초 이상 걸린다. `Shutdown`은 worker를 Join하므로 Cancel 모드여도 그만큼 기다린다. 큰 입력은 블록 컨테이너(후속)를
    쓴다.
- **소멸**: 진행 중인 task의 소멸자는 취소를 요청하고 기다린다. 대기 중인 작업의 취소는 worker가 처리하므로 모든 worker가
  막혀 있으면 소멸자도 기다린다. 같은 scheduler의 worker(다른 작업의 body·capture 정리 포함)에서는 기다릴 수 없어 Fatal이므로
  task를 다른 작업의 capture가 소유하게 하지 않는다. 다른 스레드는 `Handle().Clone()`으로 얻은 handle로만 기다린다.
- **용량**: scheduler의 `maxInFlight`(기본 256)를 넘으면 CapacityExceeded다. caller가 제출 속도를 조절한다. task는 이동할
  수 없어 `Array` 원소가 될 수 없으므로 C 배열이나 `UniquePtr`로 둔다.

## 6. Godot·UE 대응

| Godot `Compression` | UE `FCompression` | Rocks |
|---|---|---|
| `compress(dst, src, size, mode)` | `CompressMemory` | `TryCompress` / `TryCompressAppend` |
| `get_max_compressed_buffer_size` | `CompressMemoryBound` | `TryGetCompressBound` |
| `decompress(dst, dst_max, src, size, mode)` | `UncompressMemory` | `TryDecompress` |
| `decompress_dynamic(vec, max, …)` | — | `TryDecompressAppend`(상한 필수) |
| 모드 FASTLZ·DEFLATE(실제는 zlib 래퍼)·ZSTD·GZIP·BROTLI(해제) | NAME_Zlib·Gzip·LZ4·Oodle | Lz4·Deflate(raw)·Zlib·Zstd·Gzip |
| 전역 static 수준 설정 | flags(BiasSpeed·BiasSize) | 호출마다 `CompressionSettings` |
| thread_local zstd 문맥 | — | 명시 `Compressor`·`Decompressor` |
| — | IoStore 내부 비동기 해제 | `TrySubmitCompress`·`TrySubmitDecompress` |
| `FileAccessCompressed`(블록·seek) | `FCompressedBuffer` | 후속 |
| `StreamPeerGZIP` | — | 후속 |

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

## 8. 구현 상태와 검증 (2026-11-12, Windows x64 로컬)

| 항목 | 결과 |
|---|---|
| 원본 무결성 | zstd 공식 `.sha256` 일치, zlib `.asc` GPG 서명 정상(지문 5ED4…AFBA), LZ4·zlib GitHub 자산 digest 일치. 저장소 원본 81개 파일의 SHA-256이 manifest와 일치 |
| 빌드 | clang 21(clang-cl)로 Debug·ASan·Release(ThinLTO, C bitcode 포함) Foundation 타깃 경고 0 |
| Compression 시험 | 17개(동기 10·비동기 7: 실행 중·늦은 취소, 같은 output 연쇄 Append 포함)가 Debug·ASan·Release에서 통과. Foundation 전체 통과 |
| 빌드 도구 시험 | `Rocks.Build.Tests` 21개 그룹 통과(C++ compile action golden Debug·Release, C17 프로필, 실제 C 빌드·실행 Debug·Release, include된 `.c` 무효화, 언어 변경 인자 거부) |
| 정책 시험 | `Rocks.ToolTests self` 72개 통과(third-party 저장소 검사와 음성 대조). `native` 96개 통과(실제 compile DB 언어·경고 감사, 지시문 무효 확인과 그 양성·음성 대조) |

처리량(로컬 1회 측정, 저장소 밖 임시 벤치): Windows x64 Release(ThinLTO), 단일 스레드, `Compressor`·`Decompressor` 재사용,
저장소 소스·문서 8.9MiB, 여러 번 중 최고값이다. 기기 사양을 고정한 반복 측정이 아니므로 기준값이 아니라 adapter가 원본 라이브러리
성능을 해치지 않는다는 확인으로 본다.

| 형식 | 수준 | 압축 비율 | 압축 MB/s | 해제 MB/s |
|---|---|---|---|---|
| Lz4 | 1 / 9 / 12 | 0.401 / 0.293 / 0.290 | 719 / 44 / 19 | 4770 / 4664 / 5018 |
| Zstd | 1 / 3 / 9 / 19 | 0.286 / 0.248 / 0.214 / 0.188 | 496 / 367 / 92 / 4.0 | 1473 / 1440 / 1576 / 1520 |
| Deflate·Zlib·Gzip | 1 / 6 / 9 | 0.314 / 0.256 / 0.254 | 171 / 46 / 17 | 480 / 483 / 517 |

미검증·후속:
- macOS ARM64·Android 실행과 결정성 golden의 교차 플랫폼 일치는 CI·실기기 결과가 생길 때까지 미검증이다.
- 저장소 안의 반복 가능한 벤치(고정 코퍼스·기기 기록), `DYNAMIC_BMI2=0`·LZ4 1.11 Android 해제 루프의 영향 측정은 후속이다.
- 루트를 include하는 시험 모듈은 증분 키를 계산할 때 ThirdParty `.c`(약 1.6MB)도 해시한다. 빌드 안 디렉터리 fingerprint
  memoize는 후속 최적화다.
