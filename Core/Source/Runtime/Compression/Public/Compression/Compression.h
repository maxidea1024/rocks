#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 압축 헬퍼(Godot Compression·UE FCompression 대응)다. 설계·근거는
// Docs/Design/Compression.ko.md를 따른다.
//
// 사용 예(일회성):
//   auto settings = CompressionSettings::TryDefault(CompressionFormat::Zstd).Value();
//   Array<uint8> packed(allocator);
//   auto packedBytes = TryCompressAppend(settings, original, packed); // packed 뒤에 붙인다
//   Array<uint8> restored(allocator);
//   auto restoredBytes =
//       TryDecompressAppend(CompressionFormat::Zstd, packed.View(), 64 << 20, restored);
//
// 사용 예(LZ4 frame: 빠른 해제와 무결성 검사가 함께 필요한 캐시·`lz4` CLI 파일):
//   auto fast = CompressionSettings::TryDefault(CompressionFormat::Lz4Frame).Value();
//   auto framed = TryCompressAppend(fast, original, packed);      // 원본 크기·checksum 기록
//   auto opened = TryDecompressAppend(CompressionFormat::Lz4Frame, packed.View(), 64 << 20,
//                                     restored);                // 손상·절단은 InvalidArgument
//
// 사용 예(크기를 아는 해제, PNG IDAT처럼 원본 크기를 헤더로 아는 경우):
//   Array<uint8> pixels(allocator);
//   pixels.TryResize(expectedSize);
//   auto target = Span<uint8>(pixels.Data(), pixels.Size());
//   auto written = TryDecompress(CompressionFormat::Zlib, idat, target, allocator);
//   // written.Value()가 expectedSize와 다르면 상위 포맷 오류로 처리한다.
//
// 공통 계약:
// - 압축·해제 함수는 쓴(붙인) byte 수를 반환한다.
// - Span 출력은 범위 밖에는 쓰지 않는다. 실패 시 내용은 미정이고, 성공 시에도 [반환값, 끝)
//   구간은 미정이다(코덱이 성능을 위해 반환 크기 뒤를 덮어쓸 수 있다). Append는 실패 시 output
//   길이를 호출 전으로 되돌린다(capacity는 늘 수 있다).
// - 오류: 출력·상한 부족 CapacityExceeded, 손상·절단·체크섬 불일치·스트림 뒤 남는 byte·빈 해제
//   입력·범위 밖 수준·source와 출력의 겹침·모르는 형식 값(등록되지 않았거나 해제가 시작된
//   사용자 등록 형식 값 포함) InvalidArgument, 문맥에 없거나 다른 사전이 필요한 입력
//   (zlib 프리셋 사전, 사전 ID가 다른 zstd frame, 형식 사전 문맥에 사전 ID 없는 frame, 사전 ID를
//   기록한 LZ4 frame), window가 구현 상한(2^31)을 넘는 zstd frame·LZ4 legacy frame·Brotli large
//   window 스트림·해제 전용 형식(Brotli)의 압축 Unsupported, 할당 실패 OutOfMemory, 크기 산술
//   overflow Overflow, 외부 코덱이 계약 밖의 실패를 보고하거나 사용자 등록 코덱이 반환 계약
//   (CompressionCodec.h)을 어김 DependencyFailed.
// - 할당은 모두 인자로 받은 Allocator를 거친다. 전역·thread_local 캐시가 없어 자유 함수는 재진입
//   가능하다. 설정 성격의 전역 상태는 사용자 등록 형식 표(CompressionCodec.h)뿐이며 내장 형식
//   경로는 이 표를 읽지 않는다. 반복 호출이 많으면 코덱 문맥을 재사용하는 Compressor·
//   Decompressor를 쓴다.
// - 같은 라이브러리 버전·설정·입력이면 64bit little-endian 대상(Windows x64·macOS ARM64·Android
//   arm64/x86_64)에서 같은 byte를 만든다(쿠킹 재현성). 비동기 API도 같은 결과를 만든다.
namespace rocks {

namespace detail {
struct CompressorState;
struct DecompressorState;
struct DecompressorAccess;
} // namespace detail

class CompressDictionary;
class DecompressDictionary;

// 이 모듈의 출력 byte를 바꾸는 결정(코덱 버전, frame 설정·분할 단위·사전 매개변수 정책, 블록
// 컨테이너 배치 등)이 바뀔 때마다 올리는 개정 번호다. 내장 형식의 TryGetCompressionFormatInfo
// revision이 이 값이다. 압축 결과를 캐시하는 쪽(쿠킹 빌드 키)은 형식 이름·revision
// (TryGetCompressionFormatInfo)·수준·사전 Digest를 키에 넣는다.
inline constexpr uint32 CompressionOutputRevision = 1;

// 압축 형식이다. 값은 프로세스 안 식별용이며 영속 포맷에 기록할 때는 상위 포맷이 별도 ID(또는 형식
// 이름)를 정한다. 0–255는 내장 형식이며 새 형식은 끝에 덧붙인다. 256 이상은 사용자 등록 형식
// (CompressionCodec.h)의 값으로, 등록 순서에 따라 실행마다 달라지므로 파일·빌드 캐시 키에는 형식
// 이름을 쓴다. 등록되지 않았거나 해제된 값은 모르는 형식 값과 같이 InvalidArgument다.
// - Lz4: LZ4 raw block. 해제가 가장 빠르다. 원본 크기·체크섬이 없어 해제할 때 크기를 알아야
// 효율적이다. 스트림을 지원하지 않는다(블록 컨테이너 CompressedBlocks의 LZ4 블록이 이 형식이다).
// - Zstd: Zstandard frame(RFC 8878). 원본 크기와 content checksum(XXH64 하위 32bit)을 기록한다.
// - Deflate: raw DEFLATE(RFC 1951). 무결성 검사가 없다.
// - Zlib: zlib 래퍼(RFC 1950, Adler-32). PNG IDAT가 이 형식이다.
// - Gzip: gzip member(RFC 1952, CRC-32·ISIZE). 해제는 연속 member를 모두 처리한다.
// - Lz4Frame: LZ4 frame(LZ4 Frame Format 1.6.1, `lz4` CLI·.lz4 파일과 호환). 64KiB linked 블록과
//   content checksum(XXH32)을 쓰고, 일회성 압축은 원본 크기도 기록한다. 무결성 검사·스트리밍
//   (CompressStream·DecompressStream)을 지원하는 대신 해제는 Lz4보다 느리다(checksum 계산 때문에
//   Lz4의 60%대, Docs/Design/Compression.ko.md 12.5절). 해제는 연속 frame과 skippable frame을 모두
//   처리한다. 사전 ID를 기록한 frame과 legacy frame(`lz4 -l`)은 Unsupported다. 블록 컨테이너 형식은
//   아니다.
// - Brotli: Brotli 스트림(RFC 7932, google/brotli 1.1.1). 해제 전용이다(WOFF2 글꼴·HTTP
//   `Content-Encoding: br` 자산 읽기용). 압축 경로(TryGetCompressionLevelRange·
//   CompressionSettings·TryGetCompressBound·Compressor·CompressStream)는 모두 Unsupported다.
//   원본 크기·checksum이 없어 손상 검출은 형식 오류에 한정된다. 스트림 하나가 입력 전체여야 하며
//   (연결 불가) 뒤에 남는 byte·절단·손상은 InvalidArgument, large window 확장 스트림(비표준, 첫
//   byte 0x11)은 Unsupported다. 표준 window는 최대 16MiB(2^24 - 16)다. 해제 작업 메모리는 ring
//   buffer(출력에 맞춰 2배씩 늘며 window 이하)와 Huffman 표(최악 약 2.6MiB)이고, ring buffer를
//   늘리는 순간 이전 것과 함께 있어 최악 약 27MiB(1.5 × 16MiB + 표)다. 작은 출력은 작은 ring
//   buffer만 쓴다. 사전 문맥·블록 컨테이너 형식은 아니다.
//
// 사용 예(WOFF2처럼 상위 포맷 헤더가 해제 크기를 알려 주는 경우). WOFF2의 해제 크기는 표
// 디렉터리의 origLength(변환하지 않은 표)·transformLength(변환한 표)의 합이다(totalSfntSize는
// 참고값):
//   // tables는 그 합만큼 확보한 버퍼, compressedTables는 표 디렉터리 뒤의 Brotli stream이다.
//   auto target = Span<uint8>(tables.Data(), tables.Size());
//   auto written = TryDecompress(CompressionFormat::Brotli, compressedTables, target, allocator);
//   // written.Value()가 합과 다르면 상위 포맷 오류로 처리한다.
enum class CompressionFormat : uint32 {
    Lz4,
    Zstd,
    Deflate,
    Zlib,
    Gzip,
    Lz4Frame,
    Brotli
};

// 형식별 압축 수준 범위다.
// - Lz4·Lz4Frame {1, 12, 1}: 1 = fast, 2 = lz4mid, 3–9 = HC, 10–12 = HC optimal parser(가장 느리고
//   작다).
// - Zstd {1, 22, 3}: 20–22(ultra)는 window가 최대 128MiB(2^27)다. 원본 크기를 아는 압축은
//   매개변수가 입력에 맞춰 줄지만, 큰 입력·크기를 모르는 스트림은 수백 MiB 작업 메모리를 쓴다(수준
//   22 약 700MiB). 해제는 DecompressStreamLimits 기본 window 상한(2^27)으로 풀린다.
// - Deflate·Zlib·Gzip {0, 9, 6}: 0 = 압축하지 않음(stored).
// - Brotli: 해제 전용이라 범위가 없다(Unsupported).
// - 사용자 등록 형식: 등록할 때 준 범위다.
struct CompressionLevelRange {
    int32 minimum = 0;
    int32 maximum = 0;
    int32 defaultLevel = 0;
};

// 모르는 형식 값이면 InvalidArgument, 해제 전용 형식(Brotli)이면 Unsupported다.
Result<CompressionLevelRange> TryGetCompressionLevelRange(CompressionFormat format) noexcept;

// 형식 이름의 최대 길이(byte)다.
inline constexpr usize MaxCompressionFormatNameBytes = 32;

// 형식의 이름·개정 번호다(내장·사용자 등록 형식 공통). 빌드 캐시 키는 형식 값 대신 이 이름과
// revision을 쓴다(사용자 등록 형식의 값은 실행마다 다를 수 있다).
// - Name(): 내장 형식은 "lz4"·"zstd"·"deflate"·"zlib"·"gzip"·"lz4frame"·"brotli",
//   사용자 등록 형식은 등록 이름("oodle.kraken" 같은 '.' 포함 이름)이다. 반환 view는 이 객체
//   안을 가리키므로 결과(Result)를 변수로 받아 그 수명 안에서 쓴다. 임시 정보 객체에서는
//   부를 수 없지만, 임시 Result의 Value().Name()은 막지 못하므로 문장이 끝나면 무효다.
// - revision: 내장 형식은 CompressionOutputRevision, 사용자 등록 형식은 등록 version이다.
// - plugin: 사용자 등록 형식인지다.
// - compresses: 압축을 지원하는지다(해제 전용 형식 Brotli는 false).
//
// 사용 예(쿠킹 빌드 키):
//   auto info = TryGetCompressionFormatInfo(settings.Format());
//   key.Append(info.Value().Name()); // 그 뒤에 revision·수준을 붙인다
struct CompressionFormatInfo {
    char nameBytes[MaxCompressionFormatNameBytes] = {};
    uint8 nameSize = 0;
    uint32 revision = 0;
    bool plugin = false;
    bool compresses = true;

    StringView Name() const& noexcept {
        return StringView(nameBytes, nameSize);
    }

    StringView Name() const&& = delete;
};

// 형식의 이름·revision이다. 모르는 값·등록되지 않았거나 해제(진행 중 포함)된 값은
// InvalidArgument다.
Result<CompressionFormatInfo> TryGetCompressionFormatInfo(CompressionFormat format) noexcept;

// 이름으로 형식을 찾는다(내장·사용자 등록 형식 공통). 형식 이름 문법(소문자 ASCII
// [a-z0-9_-]로 된 비지 않은 조각을 '.'로 이은 1–32byte) 위반은 InvalidArgument이며 대문자가
// 섞인 이름도 여기에 든다. 문법에 맞지만 그런 형식이 없거나 해제 중이면 NotFound다.
//
// 사용 예(설정 파일의 "zstd"·"oodle.kraken" 같은 이름을 형식 값으로):
//   auto format = TryFindCompressionFormat(StringView("zstd"));
Result<CompressionFormat> TryFindCompressionFormat(StringView name) noexcept;

// 검증된 형식·수준 쌍이다. 생성 함수만 만들 수 있어 압축 함수는 만들 때 유효했던 설정을 받는다.
// 사용자 등록 형식은 설정을 만든 뒤 해제가 시작될 수 있으며, 그 뒤 이 설정을 쓰는 압축 함수·
// 문맥 생성·비동기 제출·스트림 생성은 InvalidArgument다.
class CompressionSettings {
public:
    // 수준이 GetCompressionLevelRange 범위 밖이거나 모르는 형식이면 InvalidArgument, 해제 전용
    // 형식(Brotli)이면 Unsupported다.
    static Result<CompressionSettings> TryMake(CompressionFormat format, int32 level) noexcept;

    // 형식의 기본 수준이다. 모르는 형식이면 InvalidArgument, 해제 전용 형식이면 Unsupported다.
    static Result<CompressionSettings> TryDefault(CompressionFormat format) noexcept;

    CompressionFormat Format() const noexcept {
        return format_;
    }

    int32 Level() const noexcept {
        return level_;
    }

private:
    CompressionSettings(CompressionFormat format, int32 level) noexcept
        : format_(format), level_(level) {
    }

    CompressionFormat format_;
    int32 level_;
};

// LZ4 블록 형식의 입력 한계(LZ4_MAX_INPUT_SIZE)다. 더 큰 입력의 Lz4 압축·bound 계산은
// InvalidArgument다. Lz4Frame은 64KiB 블록으로 나누므로 이 한계가 없다.
inline constexpr usize Lz4MaxInputBytes = 0x7E000000;

// sourceBytes를 압축한 결과의 최악 크기다. 이 크기 이상의 destination을 주면 TryCompress는
// CapacityExceeded로 실패하지 않는다. Lz4 입력 한계 초과·모르는 형식은 InvalidArgument, 해제
// 전용 형식(Brotli)은 Unsupported, 계산 overflow는 Overflow다. Lz4Frame은 헤더(최대 19byte) + 64KiB
// 블록마다 4byte + 원본 + endmark·checksum 8byte다.
Result<usize> TryGetCompressBound(CompressionFormat format, usize sourceBytes) noexcept;

// source를 압축해 destination 앞부분에 쓰고 쓴 byte 수를 반환한다. scratch는 코덱 작업 메모리이며
// 호출 안에서 모두 반환한다. 필요한 크기는 TryGetCompressBound로 미리 구한다.
Result<usize> TryCompress(const CompressionSettings& settings, Span<const uint8> source,
                          Span<uint8> destination, Allocator& scratch) noexcept;

// source를 압축해 output 뒤에 붙인다. 작업 메모리는 output.GetAllocator()를 쓴다. source가 output
// 저장소
// ([Data, Data + Capacity))를 가리키면 InvalidArgument다(성장 중 재할당으로 무효가 되기 때문이다).
Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source,
                                Array<uint8>& output) noexcept;

// 작업 메모리 할당기를 따로 지정하는 변형이다.
Result<usize> TryCompressAppend(const CompressionSettings& settings, Span<const uint8> source,
                                Array<uint8>& output, Allocator& scratch) noexcept;

// 해제 크기를 아는 경우의 해제다. 해제한 byte 수를 반환하며 결과가 destination보다 크면
// CapacityExceeded다. 정확히 destination 크기인 결과는 성공이다. Lz4는 원본 크기가 기록되지
// 않으므로 이 함수가 가장 빠른 경로다.
Result<usize> TryDecompress(CompressionFormat format, Span<const uint8> source,
                            Span<uint8> destination, Allocator& scratch) noexcept;

// 해제 크기를 모르는 경우의 해제다. 결과를 output 뒤에 붙이며 maxOutputBytes를 넘으면
// CapacityExceeded다(악성 입력의 메모리 폭주 방지). 작업 메모리는 output.GetAllocator()를 쓴다.
// - Zstd는 frame에 기록된 크기로 한 번에 확보한다. 크기가 없는 frame은 블록 수로 상한을 구한다.
// - Deflate·Zlib·Gzip은 출력을 점진적으로 늘린다.
// - Lz4Frame은 첫 frame에 기록된 크기(없으면 64KiB)를 먼저 확보하고, 출력이 더 나올 때만 두 배씩
//   늘린다. 먼저 잡는 크기는 상한과 입력 크기 × 256 + 64KiB를 넘지 않는다.
// - Brotli는 원본 크기가 없어 64KiB(상한 이하)를 먼저 잡고 출력이 더 나올 때만 두 배씩 늘린다.
// - Lz4는 크기 정보가 없어 출력이 부족하면 두 배로 늘려 처음부터 다시 해제한다(크기를 알면
//   TryDecompress를 쓴다). 사용자 등록 형식도 같은 방식이다(CompressionCodec.h). 이 두 형식은
//   손상 입력에서도 상한까지 출력을 늘려 볼 수 있어, 늘리다 할당이 실패하면 OutOfMemory다.
Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output) noexcept;

// 작업 메모리 할당기를 따로 지정하는 변형이다.
Result<usize> TryDecompressAppend(CompressionFormat format, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output,
                                  Allocator& scratch) noexcept;

// 사전을 쓰는 일회성 변형이다(CompressionDictionary.h). 형식·수준은 사전이 정하며 계약은 사전 없는
// 함수와 같다. 빈 사전 handle은 StaleHandle이다. 사전 준비(CDict·DDict)는 handle이 가지므로
// 호출마다 드는 비용은 사전 없는 함수와 같은 문맥 생성뿐이다.
Result<usize> TryCompress(const CompressDictionary& dictionary, Span<const uint8> source,
                          Span<uint8> destination, Allocator& scratch) noexcept;
Result<usize> TryCompressAppend(const CompressDictionary& dictionary, Span<const uint8> source,
                                Array<uint8>& output) noexcept;
Result<usize> TryCompressAppend(const CompressDictionary& dictionary, Span<const uint8> source,
                                Array<uint8>& output, Allocator& scratch) noexcept;
Result<usize> TryDecompress(const DecompressDictionary& dictionary, Span<const uint8> source,
                            Span<uint8> destination, Allocator& scratch) noexcept;
Result<usize> TryDecompressAppend(const DecompressDictionary& dictionary, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output) noexcept;
Result<usize> TryDecompressAppend(const DecompressDictionary& dictionary, Span<const uint8> source,
                                  usize maxOutputBytes, Array<uint8>& output,
                                  Allocator& scratch) noexcept;

// 반복 압축용 문맥이다. 코덱 상태(zstd 문맥·LZ4 상태·LZ4 frame 문맥·zlib deflate 상태)를 한 번
// 할당해 재사용하므로 작은 입력을 많이 압축할 때 빠르다. 결과는 자유 함수와 byte 단위로 같다.
// LZ4 frame 문맥은 블록 버퍼·LZ4 상태를 첫 압축에서 할당하고 이후 재사용한다.
// - 이동 전용이다. 이동된 뒤의 객체에 호출하면 StaleHandle이다.
// - 한 객체는 동시에 한 스레드만 쓴다. 할당기는 객체보다 오래 살아야 한다.
// - 실패(용량 부족·OOM·취소) 뒤에도 다음 호출은 새 문맥과 같은 결과를 낸다.
// - 사용자 등록 형식의 문맥은 살아 있는 동안 사용권을 쥔다. 그동안 그 형식의
//   TryUnregisterCompressionCodec은 Busy이고, 해제가 시작된 뒤에도 이 문맥은 계속 쓸 수 있다.
//
// 사용 예:
//   auto compressor = Compressor::TryCreate(settings, allocator);
//   for (auto& chunk : chunks) {
//       auto bytes = compressor.Value().TryCompressAppend(chunk, packed);
//   }
class Compressor {
public:
    Compressor() noexcept = default;
    ~Compressor() noexcept;
    Compressor(Compressor&& other) noexcept;
    Compressor& operator=(Compressor&& other) noexcept;
    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    // 실패: OutOfMemory, 외부 코덱 초기화 실패 DependencyFailed, 설정을 만든 뒤 해제가 시작된
    // 사용자 등록 형식 InvalidArgument.
    static Result<Compressor> TryCreate(const CompressionSettings& settings,
                                        Allocator& allocator) noexcept;

    // 사전을 쓰는 문맥이다. 형식·수준은 사전이 정하고, frame에 사전 ID(있으면)를 기록한다. 문맥이
    // 사전 handle 복사본을 가진다. 빈 handle은 StaleHandle이다.
    static Result<Compressor> TryCreate(const CompressDictionary& dictionary,
                                        Allocator& allocator) noexcept;

    Result<usize> TryCompress(Span<const uint8> source, Span<uint8> destination) noexcept;
    Result<usize> TryCompressAppend(Span<const uint8> source, Array<uint8>& output) noexcept;

private:
    explicit Compressor(detail::CompressorState* state) noexcept : state_(state) {
    }

    void Release() noexcept;

    detail::CompressorState* state_ = nullptr;
};

// 반복 해제용 문맥이다. 계약은 Compressor와 같다.
class Decompressor {
public:
    Decompressor() noexcept = default;
    ~Decompressor() noexcept;
    Decompressor(Decompressor&& other) noexcept;
    Decompressor& operator=(Decompressor&& other) noexcept;
    Decompressor(const Decompressor&) = delete;
    Decompressor& operator=(const Decompressor&) = delete;

    static Result<Decompressor> TryCreate(CompressionFormat format, Allocator& allocator) noexcept;

    // 사전을 쓰는 문맥이다(DecompressDictionary의 frame 검사 계약). 빈 handle은 StaleHandle이다.
    static Result<Decompressor> TryCreate(const DecompressDictionary& dictionary,
                                          Allocator& allocator) noexcept;

    Result<usize> TryDecompress(Span<const uint8> source, Span<uint8> destination) noexcept;

    // 작업 메모리는 문맥의 할당기를 쓴다. output 성장은 output의 할당기를 쓴다.
    Result<usize> TryDecompressAppend(Span<const uint8> source, usize maxOutputBytes,
                                      Array<uint8>& output) noexcept;

private:
    friend struct detail::DecompressorAccess;

    explicit Decompressor(detail::DecompressorState* state) noexcept : state_(state) {
    }

    void Release() noexcept;

    detail::DecompressorState* state_ = nullptr;
};

} // namespace rocks
