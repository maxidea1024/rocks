#pragma once

#include <Compression/Compression.h>
#include <Compression/CompressionStream.h>

// 압축 모듈 내부 경계다. 공개 API(Compression.cpp)·비동기(AsyncCompression.cpp)와 코덱
// adapter(Lz4Codec.cpp· ZstdCodec.cpp·ZlibCodec.cpp) 사이의 계약만 둔다. 외부 코덱 헤더는 각
// adapter 번역 단위만 포함한다.
namespace rocks::detail {

// 취소 확인·32bit 길이 분할의 단위다. Deflate·Zlib·Gzip은 입력 소비나 출력 생성이, zstd 압축은 입력
// 소비가, zstd 해제는 입력 소비가 이만큼 진행될 때마다 취소를 확인한다(zstd 해제는 stable
// output이라 출력을 나눌 수 없다). Lz4는 한 번의 코덱 호출이다. 동기·비동기가 같은 값을 써야 결과
// byte가 같다(zstd·deflate 스트림 경계가 이 값에 따른다). zstd 최대 블록 크기(128KiB)의 배수여야
// 한다(ZstdCodec.cpp의 static_assert).
inline constexpr usize CompressionChunkBytes = usize{1} << 20;

// 코덱 작업 메모리 정렬이다. zstd·zlib·LZ4 상태는 malloc 수준 정렬을 가정한다.
inline constexpr usize CodecAlignment = 16;

// 협력적 취소 확인이다. 동기 호출은 빈 probe(cancelled == nullptr)를 넘겨 취소되지 않는다.
struct CompressionProbe {
    bool (*cancelled)(void* context) noexcept = nullptr;
    void* context = nullptr;

    bool IsCancelled() const noexcept {
        return cancelled != nullptr && cancelled(context);
    }
};

// Allocator 경유 코덱 할당이다. 0 byte 요청도 유효 주소를 돌려준다(코덱은 nullptr을 실패로 본다).
void* CodecAllocate(Allocator& allocator, usize bytes) noexcept;
void CodecFree(Allocator& allocator, void* address) noexcept;

// 빈 Span의 nullptr 대신 넘길 주소다. 코덱은 0 byte를 읽거나 쓰지 않지만 nullptr을 거부할 수
// 있다(zlib next_out).
const uint8* NonNullSource(Span<const uint8> source) noexcept;
uint8* NonNullDestination(Span<uint8> destination) noexcept;

// [first, first + firstSize)와 [second, second + secondSize)가 겹치는지다. 빈 구간은 겹치지 않는다.
bool Overlaps(const void* first, usize firstSize, const void* second, usize secondSize) noexcept;

// Append 출력의 쓰기 구간이 source와 겹치지 않는지 확인한다. 성장 재할당까지 고려해 capacity 전체와
// 비교한다.
bool OverlapsArrayStorage(Span<const uint8> source, const Array<uint8>& output) noexcept;

// 압축·해제 문맥이다. 형식별 코덱 상태는 adapter가 소유한다.
struct CompressorState {
    CompressionFormat format;
    int32 level;
    Allocator* allocator;
    void* codec;
};

struct DecompressorState {
    CompressionFormat format;
    Allocator* allocator;
    void* codec;
};

// zstdChecksum: zstd frame에 content checksum을 넣을지다. 블록 컨테이너는 자체 CRC가 있어 끈다.
Result<CompressorState*> CreateCompressorState(CompressionFormat format, int32 level,
                                               Allocator& allocator,
                                               bool zstdChecksum = true) noexcept;
void DestroyCompressorState(CompressorState* state) noexcept;
Result<DecompressorState*> CreateDecompressorState(CompressionFormat format,
                                                   Allocator& allocator) noexcept;
void DestroyDecompressorState(DecompressorState* state) noexcept;

// 공개 API·비동기가 함께 쓰는 단일 구현이다. 인자 검증(겹침·한계)을 포함한다.
Result<usize> CompressInto(CompressorState& state, Span<const uint8> source,
                           Span<uint8> destination, const CompressionProbe& probe) noexcept;
Result<usize> CompressAppend(CompressorState& state, Span<const uint8> source, Array<uint8>& output,
                             const CompressionProbe& probe) noexcept;
Result<usize> DecompressInto(DecompressorState& state, Span<const uint8> source,
                             Span<uint8> destination, const CompressionProbe& probe) noexcept;
Result<usize> DecompressAppend(DecompressorState& state, Span<const uint8> source,
                               usize maxOutputBytes, Array<uint8>& output,
                               const CompressionProbe& probe) noexcept;

// 일회성 작업: 문맥을 scratch로 만들고 작업 뒤 반환한다.
Result<usize> CompressAppendOnce(const CompressionSettings& settings, Span<const uint8> source,
                                 Array<uint8>& output, Allocator& scratch,
                                 const CompressionProbe& probe) noexcept;
Result<usize> DecompressIntoOnce(CompressionFormat format, Span<const uint8> source,
                                 Span<uint8> destination, Allocator& scratch,
                                 const CompressionProbe& probe) noexcept;
Result<usize> DecompressAppendOnce(CompressionFormat format, Span<const uint8> source,
                                   usize maxOutputBytes, Array<uint8>& output, Allocator& scratch,
                                   const CompressionProbe& probe) noexcept;

// Append 출력의 기존 길이 base 뒤로 길이를 맞춘다. 축소는 실패하지 않는다.
void RestoreLength(Array<uint8>& output, usize base) noexcept;

// --- 코덱 adapter. 검증을 마친 입력만 받는다. ---

namespace lz4 {
Result<usize> Bound(usize sourceBytes) noexcept;
Result<void*> CreateCompressor(int32 level, Allocator& allocator) noexcept;
void DestroyCompressor(void* codec, int32 level, Allocator& allocator) noexcept;
Result<usize> Compress(void* codec, int32 level, Span<const uint8> source,
                       Span<uint8> destination) noexcept;
Result<usize> Decompress(Span<const uint8> source, Span<uint8> destination) noexcept;
Result<usize> DecompressAppend(Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output) noexcept;
} // namespace lz4

// --- 스트리밍(10절) ---

enum class StreamFlush : uint8 {
    None,
    Sync,
    End
};

// 해제 스트림의 경계 추적 상태다. adapter의 단계 함수가 갱신한다.
struct StreamDecodeState {
    bool sawStream = false;  // frame·member를 하나 이상 끝까지 받았다
    bool atBoundary = false; // 지금까지의 입력이 완결된 스트림(들)이다
    bool ended = false;      // Deflate·Zlib: 스트림이 끝났다(뒤 byte는 손상)
    bool outputPending =
        false; // 지난 호출이 출력 공간을 다 채웠다(코덱 안에 출력이 남았을 수 있다)
    // zstd: frame 시작에서 헤더를 모아 window를 먼저 검사한다(단일 패스 경로가 상한 검사를
    // 건너뛰므로).
    bool atFrameStart = true;
    uint32 headerBytes = 0;
    uint8 header[32] = {};
};

namespace zstd {
Result<usize> Bound(usize sourceBytes) noexcept;
Result<void*> CreateCompressor(int32 level, Allocator& allocator, bool checksum = true) noexcept;
// 스트림용 문맥이다(stable buffer를 쓰지 않는다).
Result<void*> CreateStreamCompressor(int32 level, uint64 sizeHint, Allocator& allocator) noexcept;
Result<void*> CreateStreamDecompressor(uint32 maxWindowLog, Allocator& allocator) noexcept;
void ResetStreamCompressor(void* codec) noexcept;
void ResetStreamDecompressor(void* codec) noexcept;
Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept;
Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      uint32 maxWindowLog, StreamDecodeState& state) noexcept;
void DestroyCompressor(void* codec) noexcept;
Result<void*> CreateDecompressor(Allocator& allocator) noexcept;
void DestroyDecompressor(void* codec) noexcept;
Result<usize> Compress(void* codec, Span<const uint8> source, Span<uint8> destination,
                       const CompressionProbe& probe) noexcept;
Result<usize> Decompress(void* codec, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept;
Result<usize> DecompressAppend(void* codec, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept;
} // namespace zstd

namespace zlib {
Result<usize> Bound(usize sourceBytes) noexcept;
// 압축 문맥의 설정으로 구한 상한이다(Bound보다 작다). 문맥이 만들 수 있는 최악 크기 이상이다.
Result<usize> StreamBound(void* codec, usize sourceBytes) noexcept;
Result<void*> CreateCompressor(CompressionFormat format, int32 level,
                               Allocator& allocator) noexcept;
Result<void*> CreateDecompressor(CompressionFormat format, Allocator& allocator) noexcept;
void Destroy(void* codec) noexcept;
Result<usize> Compress(void* codec, Span<const uint8> source, Span<uint8> destination,
                       const CompressionProbe& probe) noexcept;
Result<usize> Decompress(void* codec, Span<const uint8> source, Span<uint8> destination,
                         const CompressionProbe& probe) noexcept;
Result<usize> DecompressAppend(void* codec, Span<const uint8> source, usize maxOutputBytes,
                               Array<uint8>& output, const CompressionProbe& probe) noexcept;

Result<void> ResetStreamCompressor(void* codec) noexcept;
Result<void> ResetStreamDecompressor(void* codec) noexcept;
Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept;
Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      StreamDecodeState& state) noexcept;

// CRC-32(Core Crc32와 같은 값). zlib의 braid·하드웨어 가속 구현을 쓴다. 길이 제한 없음.
uint32 Crc32(Span<const uint8> data, uint32 previous = 0) noexcept;
} // namespace zlib

} // namespace rocks::detail
