#pragma once

#include <Compression/Compression.h>
#include <Compression/CompressionDictionary.h>
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

// 사전 코덱(CDict·DDict)의 할당 경로다. zstd create 함수는 OOM과 손상을 모두 NULL로 알리므로 할당
// 실패를 따로 기록한다. 코덱이 해제할 때도 쓰므로 코덱보다 오래 살아야 한다(사전 상태 안에 둔다).
struct CodecMemory {
    Allocator* allocator = nullptr;
    bool allocationFailed = false;
};

// 공유 사전 handle의 대상이다(CompressDictionary·DecompressDictionary). 만든 뒤에는 바뀌지 않는다.
struct DictionaryState {
    CompressionFormat format = CompressionFormat::Zstd;
    bool compressing = false; // codec이 CDict(압축)인지 DDict(해제)인지
    int32 level = 0;          // 압축 사전의 수준
    uint32 id = 0;            // frame에 기록되는 사전 ID(raw 사전은 0)
    usize bytes = 0;
    Sha256Digest digest;
    CodecMemory memory;
    void* codec = nullptr;

    DictionaryState() noexcept = default;
    ~DictionaryState() noexcept;
    DictionaryState(const DictionaryState&) = delete;
    DictionaryState& operator=(const DictionaryState&) = delete;
};

// 공개 handle의 내부 상태 접근이다.
struct DictionaryAccess {
    static const SharedPtr<DictionaryState>& State(const CompressDictionary& dictionary) noexcept {
        return dictionary.state_;
    }

    static const SharedPtr<DictionaryState>&
    State(const DecompressDictionary& dictionary) noexcept {
        return dictionary.state_;
    }
};

// 압축·해제 문맥이다. 형식별 코덱 상태는 adapter가 소유한다. dictionary는 사전 문맥이 가진 handle
// 복사본이다(사전 없으면 빈 handle).
struct CompressorState {
    CompressionFormat format;
    int32 level;
    Allocator* allocator;
    void* codec;
    SharedPtr<DictionaryState> dictionary;
};

// active는 지금 코덱에 걸린 사전이다. 사전 문맥은 dictionary와 같고, 블록 컨테이너 읽기는 읽을
// 때마다 reader의 사전을 빌려 건다(BindDecompressorDictionary).
struct DecompressorState {
    CompressionFormat format;
    Allocator* allocator;
    void* codec;
    SharedPtr<DictionaryState> dictionary;
    const DictionaryState* active;
};

struct DecompressorAccess {
    static DecompressorState* State(Decompressor& decompressor) noexcept {
        return decompressor.state_;
    }
};

// zstdChecksum: zstd frame에 content checksum을 넣을지다. 블록 컨테이너는 자체 CRC가 있어 끈다.
// dictionary가 있으면 형식·수준은 사전의 값이며 format·level 인자는 무시한다.
Result<CompressorState*> CreateCompressorState(
    CompressionFormat format, int32 level, Allocator& allocator, bool zstdChecksum = true,
    const SharedPtr<DictionaryState>& dictionary = SharedPtr<DictionaryState>()) noexcept;
void DestroyCompressorState(CompressorState* state) noexcept;
Result<DecompressorState*> CreateDecompressorState(
    CompressionFormat format, Allocator& allocator,
    const SharedPtr<DictionaryState>& dictionary = SharedPtr<DictionaryState>()) noexcept;
void DestroyDecompressorState(DecompressorState* state) noexcept;

// 해제 문맥에 사전을 건다(nullptr이면 뗀다). 걸 때는 항상 다시 걸고, 떼어진 문맥을 떼는 것만
// 생략한다. 사전을 가진 문맥(dictionary 보유)에는 쓰지 않는다. dictionary는 다음 Bind까지 살아
// 있어야 한다.
Result<void> BindDecompressorDictionary(DecompressorState& state,
                                        const DictionaryState* dictionary) noexcept;

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

// 일회성 작업: 문맥을 scratch로 만들고 작업 뒤 반환한다. dictionary가 있으면 사전 문맥이다(형식·
// 수준은 사전의 값).
Result<usize> CompressAppendOnce(
    const CompressionSettings& settings, Span<const uint8> source, Array<uint8>& output,
    Allocator& scratch, const CompressionProbe& probe,
    const SharedPtr<DictionaryState>& dictionary = SharedPtr<DictionaryState>()) noexcept;
Result<usize> DecompressIntoOnce(
    CompressionFormat format, Span<const uint8> source, Span<uint8> destination, Allocator& scratch,
    const CompressionProbe& probe,
    const SharedPtr<DictionaryState>& dictionary = SharedPtr<DictionaryState>()) noexcept;
Result<usize> DecompressAppendOnce(
    CompressionFormat format, Span<const uint8> source, usize maxOutputBytes, Array<uint8>& output,
    Allocator& scratch, const CompressionProbe& probe,
    const SharedPtr<DictionaryState>& dictionary = SharedPtr<DictionaryState>()) noexcept;

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
// dictionary가 있으면 사전(CDict)을 걸고 frame에 사전 ID를 기록한다(수준은 사전의 값).
Result<void*> CreateCompressor(int32 level, Allocator& allocator, bool checksum = true,
                               const DictionaryState* dictionary = nullptr) noexcept;
// 스트림용 문맥이다(stable buffer를 쓰지 않는다).
Result<void*> CreateStreamCompressor(int32 level, uint64 sizeHint, Allocator& allocator,
                                     const DictionaryState* dictionary = nullptr) noexcept;
Result<void*> CreateStreamDecompressor(uint32 maxWindowLog, Allocator& allocator,
                                       const DictionaryState* dictionary = nullptr) noexcept;

// 사전 코덱이다. bytes를 복사한 CDict(수준 고정)·DDict를 만든다. memory는 반환한 코덱이 해제될
// 때까지 유효해야 한다. 오류: 할당 실패 OutOfMemory, 형식 사전 손상·사전 ID 0 InvalidArgument.
Result<void*> CreateCompressDictionary(Span<const uint8> bytes, int32 level,
                                       CodecMemory& memory) noexcept;
Result<void*> CreateDecompressDictionary(Span<const uint8> bytes, CodecMemory& memory) noexcept;
void DestroyCompressDictionary(void* dictionary) noexcept;
void DestroyDecompressDictionary(void* dictionary) noexcept;
uint32 CompressDictionaryId(const void* dictionary) noexcept;
uint32 DecompressDictionaryId(const void* dictionary) noexcept;
usize CompressDictionaryMemory(const void* dictionary) noexcept;
usize DecompressDictionaryMemory(const void* dictionary) noexcept;
Result<uint32> FrameDictionaryId(Span<const uint8> frame) noexcept;
// 해제 문맥의 사전을 바꾼다(nullptr이면 뗀다).
Result<void> BindDecompressDictionary(void* codec, const DictionaryState* dictionary) noexcept;
void ResetStreamCompressor(void* codec) noexcept;
void ResetStreamDecompressor(void* codec) noexcept;
Result<StreamProgress> CompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                    StreamFlush flush) noexcept;
// requiredDictionaryId: 형식 사전 문맥이면 그 사전 ID다. 사전 ID 없는 frame을 Unsupported로
// 거부한다(0이면 검사하지 않는다).
Result<StreamProgress> DecompressStep(void* codec, Span<const uint8> input, Span<uint8> output,
                                      uint32 maxWindowLog, uint32 requiredDictionaryId,
                                      StreamDecodeState& state) noexcept;
void DestroyCompressor(void* codec) noexcept;
Result<void*> CreateDecompressor(Allocator& allocator,
                                 const DictionaryState* dictionary = nullptr) noexcept;
void DestroyDecompressor(void* codec) noexcept;
// dictionary는 CreateCompressor에 준 사전이다(frame마다 수준 매개변수를 다시 정한다).
Result<usize> Compress(void* codec, const DictionaryState* dictionary, Span<const uint8> source,
                       Span<uint8> destination, const CompressionProbe& probe) noexcept;
Result<usize> Decompress(void* codec, uint32 requiredDictionaryId, Span<const uint8> source,
                         Span<uint8> destination, const CompressionProbe& probe) noexcept;
Result<usize> DecompressAppend(void* codec, uint32 requiredDictionaryId, Span<const uint8> source,
                               usize maxOutputBytes, Array<uint8>& output,
                               const CompressionProbe& probe) noexcept;
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
