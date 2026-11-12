#pragma once

#include <Compression/Compression.h>

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

Result<CompressorState*> CreateCompressorState(CompressionFormat format, int32 level,
                                               Allocator& allocator) noexcept;
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

namespace zstd {
Result<usize> Bound(usize sourceBytes) noexcept;
Result<void*> CreateCompressor(int32 level, Allocator& allocator) noexcept;
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
} // namespace zlib

} // namespace rocks::detail
