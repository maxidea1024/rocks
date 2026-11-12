#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>

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
//   입력·범위 밖 수준·source와 출력의 겹침 InvalidArgument, 문맥에 없거나 다른 사전이 필요한 입력
//   (zlib 프리셋 사전, 사전 ID가 다른 zstd frame, 형식 사전 문맥에 사전 ID 없는 frame)과 window가
//   구현 상한(2^31)을 넘는 zstd frame Unsupported, 할당 실패 OutOfMemory, 크기 산술 overflow
//   Overflow, 외부 코덱이 계약 밖의 실패를 보고 DependencyFailed.
// - 할당은 모두 인자로 받은 Allocator를 거친다. 전역·thread_local 캐시가 없어 자유 함수는 재진입
//   가능하다. 반복 호출이 많으면 코덱 문맥을 재사용하는 Compressor·Decompressor를 쓴다.
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
// 컨테이너 배치 등)이 바뀔 때마다 올리는 개정 번호다. 압축 결과를 캐시하는 쪽(쿠킹 빌드 키)은
// 형식·수준·사전 Digest와 함께 이 값을 키에 넣는다.
inline constexpr uint32 CompressionOutputRevision = 1;

// 압축 형식이다. 값은 프로세스 안 식별용이며 영속 포맷에 기록할 때는 상위 포맷이 별도 ID를 정한다.
// - Lz4: LZ4 raw block. 해제가 가장 빠르다. 원본 크기·체크섬이 없어 해제할 때 크기를 알아야
// 효율적이다.
// - Zstd: Zstandard frame(RFC 8878). 원본 크기와 content checksum(XXH64 하위 32bit)을 기록한다.
// - Deflate: raw DEFLATE(RFC 1951). 무결성 검사가 없다.
// - Zlib: zlib 래퍼(RFC 1950, Adler-32). PNG IDAT가 이 형식이다.
// - Gzip: gzip member(RFC 1952, CRC-32·ISIZE). 해제는 연속 member를 모두 처리한다.
enum class CompressionFormat : uint8 {
    Lz4,
    Zstd,
    Deflate,
    Zlib,
    Gzip
};

// 형식별 압축 수준 범위다.
// - Lz4 {1, 12, 1}: 1 = fast, 2 = lz4mid, 3–9 = HC, 10–12 = HC optimal parser(가장 느리고 작다).
// - Zstd {1, 19, 3}: 20–22(ultra)는 수백 MiB 작업 메모리가 필요해 지원하지 않는다.
// - Deflate·Zlib·Gzip {0, 9, 6}: 0 = 압축하지 않음(stored).
struct CompressionLevelRange {
    int32 minimum = 0;
    int32 maximum = 0;
    int32 defaultLevel = 0;
};

// 모르는 형식 값이면 InvalidArgument다.
Result<CompressionLevelRange> TryGetCompressionLevelRange(CompressionFormat format) noexcept;

// 검증된 형식·수준 쌍이다. 생성 함수만 만들 수 있어 압축 함수는 항상 유효한 설정을 받는다.
class CompressionSettings {
public:
    // 수준이 GetCompressionLevelRange 범위 밖이거나 모르는 형식이면 InvalidArgument다.
    static Result<CompressionSettings> TryMake(CompressionFormat format, int32 level) noexcept;

    // 형식의 기본 수준이다. 모르는 형식이면 InvalidArgument다.
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
// InvalidArgument다.
inline constexpr usize Lz4MaxInputBytes = 0x7E000000;

// sourceBytes를 압축한 결과의 최악 크기다. 이 크기 이상의 destination을 주면 TryCompress는
// CapacityExceeded로 실패하지 않는다. Lz4 입력 한계 초과·모르는 형식은 InvalidArgument, 계산
// overflow는 Overflow다.
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
// - Lz4는 크기 정보가 없어 출력이 부족하면 두 배로 늘려 처음부터 다시 해제한다(크기를 알면
// TryDecompress를 쓴다).
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

// 반복 압축용 문맥이다. 코덱 상태(zstd 문맥·LZ4 상태·zlib deflate 상태)를 한 번 할당해 재사용하므로
// 작은 입력을 많이 압축할 때 빠르다. 결과는 자유 함수와 byte 단위로 같다.
// - 이동 전용이다. 이동된 뒤의 객체에 호출하면 StaleHandle이다.
// - 한 객체는 동시에 한 스레드만 쓴다. 할당기는 객체보다 오래 살아야 한다.
// - 실패(용량 부족·OOM·취소) 뒤에도 다음 호출은 새 문맥과 같은 결과를 낸다.
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

    // 실패: OutOfMemory, 외부 코덱 초기화 실패 DependencyFailed.
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
