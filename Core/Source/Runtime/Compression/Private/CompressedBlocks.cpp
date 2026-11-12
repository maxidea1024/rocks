#include "CompressionInternal.h"

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>

namespace rocks {

namespace {

constexpr uint8 Magic[4] = {'R', 'C', 'B', 'K'};
// 2: 헤더 48byte(사전 bit·지문). 40byte 헤더의 1은 지원하지 않는 version(Unsupported)이다.
constexpr uint16 FormatVersion = 2;
constexpr usize EntryBytes = 8;
// indexCrc32가 덮는 헤더 앞부분이다(CRC 필드 앞까지).
constexpr usize CrcCoveredHeaderBytes = 44;
// flags bit0: 블록이 사전으로 압축되었다(헤더 32의 dictionaryFingerprint가 그 사전이다).
constexpr uint32 FlagDictionary = 1;
constexpr uint64 MaxBlockCount = 0xffffffffull;
// 블록 수가 이 값 이하이거나 원본이 ParallelMinimumBytes보다 작으면 병렬 API도 순차로 처리한다
// (helper 제출·worker 깨우기 비용이 일보다 크다).
constexpr uint64 SequentialBlockLimit = 4;
constexpr usize ParallelMinimumBytes = usize{1} << 20;
// helper 하나가 맡을 원본의 최소 크기다. helper 수는 원본 / 이 값을 넘지 않는다.
constexpr usize ParticipantMinimumBytes = usize{512} << 10;
// 병렬 job 하나의 최대 원본 크기다(취소·실패 반응성과 job 하나의 결과 메모리를 묶는다. 결과는 모든
// job이 끝날 때까지 보관하므로 전체 메모리는 압축 결과 크기만큼이다).
constexpr usize MaxJobRawBytes = usize{16} << 20;
constexpr uint32 MaxHelpers = 64;

// reader 고유 번호의 원천이다. 캐시가 해제된 reader의 주소를 재사용한 새 reader와 섞이지 않도록
// 주소 대신 번호로 식별한다. 64bit라 감기지 않는다.
uint64 ReaderSerial = 0;

uint8 CodecId(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Lz4:
        return 1;
    case CompressionFormat::Zstd:
        return 2;
    case CompressionFormat::Deflate:
        return 3;
    case CompressionFormat::Zlib:
    case CompressionFormat::Gzip:
    case CompressionFormat::Lz4Frame:
        // 래퍼·frame이 컨테이너(블록 크기·CRC)와 중복이다. LZ4 블록은 raw block(Lz4)으로 둔다.
        return 0;
    }
    return 0;
}

bool IsBlockFormat(CompressionFormat format) noexcept {
    return CodecId(format) != 0;
}

bool IsBlockSizeLog2(uint32 blockSizeLog2) noexcept {
    return blockSizeLog2 >= MinCompressedBlockSizeLog2 &&
           blockSizeLog2 <= MaxCompressedBlockSizeLog2;
}

uint64 BlockCountFor(uint64 rawSize, uint32 blockSizeLog2) noexcept {
    const uint64 mask = (uint64{1} << blockSizeLog2) - 1;
    return (rawSize >> blockSizeLog2) + ((rawSize & mask) != 0 ? 1 : 0);
}

// index번 블록의 원본 크기다. 마지막 블록만 짧을 수 있다.
usize BlockRawBytes(uint64 rawSize, uint32 blockSizeLog2, uint32 count, uint32 index) noexcept {
    const uint64 blockSize = uint64{1} << blockSizeLog2;
    return static_cast<usize>(index + 1 < count ? blockSize
                                                : rawSize - uint64{count - 1} * blockSize);
}

usize IndexBytesFor(uint32 count) noexcept {
    return CompressedBlocksHeaderBytes + EntryBytes * count;
}

uint64 Smaller(uint64 first, uint64 second) noexcept {
    return first < second ? first : second;
}

uint64 Larger(uint64 first, uint64 second) noexcept {
    return first > second ? first : second;
}

struct Header {
    CompressionFormat format = CompressionFormat::Zstd;
    uint32 blockSizeLog2 = 0;
    uint64 rawSize = 0;
    uint32 blockCount = 0;
    uint64 payloadSize = 0;
    uint32 indexCrc = 0;
    bool dictionary = false;
    uint64 fingerprint = 0;
};

// 사전 지문은 사전 SHA-256의 앞 8byte다. 헤더에는 그 byte를 그대로 쓰고 little-endian 값으로
// 비교한다.
uint64 FingerprintOf(const detail::DictionaryState& dictionary) noexcept {
    return LoadLittleEndian<uint64>(dictionary.digest.bytes);
}

// 헤더와 블록 표를 쓰고 CRC를 계산한다. out은 IndexBytesFor(count) byte다.
void WriteIndex(uint8* out, CompressionFormat format, uint32 blockSizeLog2, uint64 rawSize,
                uint32 count, uint64 payloadSize, const uint8* table,
                const detail::DictionaryState* dictionary) noexcept {
    platform::MemoryCopy(out, Magic, sizeof(Magic));
    StoreLittleEndian<uint16>(out + 4, FormatVersion);
    out[6] = CodecId(format);
    out[7] = static_cast<uint8>(blockSizeLog2);
    StoreLittleEndian<uint64>(out + 8, rawSize);
    StoreLittleEndian<uint32>(out + 16, count);
    StoreLittleEndian<uint32>(out + 20, dictionary != nullptr ? FlagDictionary : 0);
    StoreLittleEndian<uint64>(out + 24, payloadSize);
    if (dictionary != nullptr) {
        platform::MemoryCopy(out + 32, dictionary->digest.bytes, 8);
    } else {
        StoreLittleEndian<uint64>(out + 32, 0);
    }
    StoreLittleEndian<uint32>(out + 40, 0);
    platform::MemoryCopy(out + CompressedBlocksHeaderBytes, table, EntryBytes * count);
    uint32 crc = detail::zlib::Crc32(Span<const uint8>(out, CrcCoveredHeaderBytes));
    crc = detail::zlib::Crc32(
        Span<const uint8>(out + CompressedBlocksHeaderBytes, EntryBytes * count), crc);
    StoreLittleEndian<uint32>(out + CrcCoveredHeaderBytes, crc);
}

// 헤더 48byte와 컨테이너 길이의 일관성을 검사한다.
Result<Header> ParseHeader(Span<const uint8> bytes, uint64 containerBytes) noexcept {
    if (bytes.Size() < CompressedBlocksHeaderBytes ||
        !platform::MemoryEquals(bytes.Data(), Magic, sizeof(Magic))) {
        return Result<Header>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* data = bytes.Data();
    if (LoadLittleEndian<uint16>(data + 4) != FormatVersion) {
        return Result<Header>::Failure(ErrorCode::Unsupported);
    }

    Header header;
    switch (data[6]) {
    case 0:
        return Result<Header>::Failure(ErrorCode::InvalidArgument);
    case 1:
        header.format = CompressionFormat::Lz4;
        break;
    case 2:
        header.format = CompressionFormat::Zstd;
        break;
    case 3:
        header.format = CompressionFormat::Deflate;
        break;
    default:
        return Result<Header>::Failure(ErrorCode::Unsupported);
    }
    // 모르는 기능 bit와 아직 지원하지 않는 조합(Zstd 아닌 사전)은 손상이 아니라 Unsupported다.
    const uint32 flags = LoadLittleEndian<uint32>(data + 20);
    if ((flags & ~FlagDictionary) != 0 ||
        ((flags & FlagDictionary) != 0 && header.format != CompressionFormat::Zstd)) {
        return Result<Header>::Failure(ErrorCode::Unsupported);
    }
    header.dictionary = (flags & FlagDictionary) != 0;
    header.fingerprint = LoadLittleEndian<uint64>(data + 32);

    header.blockSizeLog2 = data[7];
    header.rawSize = LoadLittleEndian<uint64>(data + 8);
    header.blockCount = LoadLittleEndian<uint32>(data + 16);
    header.payloadSize = LoadLittleEndian<uint64>(data + 24);
    header.indexCrc = LoadLittleEndian<uint32>(data + CrcCoveredHeaderBytes);
    if (!IsBlockSizeLog2(header.blockSizeLog2) || LoadLittleEndian<uint32>(data + 40) != 0 ||
        (!header.dictionary && header.fingerprint != 0) ||
        BlockCountFor(header.rawSize, header.blockSizeLog2) != header.blockCount) {
        return Result<Header>::Failure(ErrorCode::InvalidArgument);
    }

    // 블록마다 1byte 이상, 원본 이하이므로 n ≤ payload ≤ rawSize다. 전체 길이가 정확히 맞아야
    // 하므로 블록 수는 컨테이너 길이 / 9를 넘지 못한다.
    const uint64 indexBytes = IndexBytesFor(header.blockCount);
    if (header.payloadSize < header.blockCount || header.payloadSize > header.rawSize ||
        containerBytes < indexBytes || containerBytes - indexBytes != header.payloadSize) {
        return Result<Header>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Header>::Success(header);
}

// 이 설정으로 압축을 시도하는지다. Deflate 수준 0은 항상 raw로 저장하므로 압축 문맥이 필요 없다.
bool Compresses(CompressionFormat format, int32 level) noexcept {
    return !(format == CompressionFormat::Deflate && level == 0);
}

// raw 블록 하나를 저장 형식으로 dst(용량 raw.Size())에 쓰고 저장 크기를 반환한다. 압축 결과가
// 원본보다 작지 않으면(용량 raw − 1에서 CapacityExceeded) raw로 저장한다. compressor는 압축을
// 시도할 때만 쓰며, 없으면 allocator로 만든다(만들지 못하면 그 오류다).
Result<usize> EncodeBlock(detail::CompressorState*& compressor, CompressionFormat format,
                          int32 level, Allocator& allocator, Span<const uint8> raw, Span<uint8> dst,
                          const SharedPtr<detail::DictionaryState>& dictionary) noexcept {
    if (raw.Size() >= 2 && Compresses(format, level)) {
        if (compressor == nullptr) {
            auto created =
                detail::CreateCompressorState(format, level, allocator, false, dictionary);
            if (!created) {
                return Result<usize>::Failure(created.Error());
            }
            compressor = created.Value();
        }
        auto written = detail::CompressInto(
            *compressor, raw, Span<uint8>(dst.Data(), raw.Size() - 1), detail::CompressionProbe{});
        if (written) {
            if (written.Value() == 0) {
                return Result<usize>::Failure(ErrorCode::DependencyFailed);
            }
            return written;
        }
        if (written.Error() != ErrorCode::CapacityExceeded) {
            return written;
        }
    }

    platform::MemoryCopy(dst.Data(), raw.Data(), raw.Size());
    return Result<usize>::Success(raw.Size());
}

void StoreEntry(uint8* table, uint32 index, usize storedSize, uint32 crc) noexcept {
    StoreLittleEndian<uint32>(table + EntryBytes * index, static_cast<uint32>(storedSize));
    StoreLittleEndian<uint32>(table + EntryBytes * index + 4, crc);
}

// 병렬 job 분배다. 호출 스레드와 worker가 같은 카운터에서 job을 가져간다. 실패가 기록되면 새 job을
// 가져가지 않는다(가져간 job은 끝까지 처리한다). job은 번호 순서로만 나가므로, 실패한 job보다 앞선
// job은 모두 처리되고 가장 앞 실패가 결정적으로 정해진다.
struct JobQueue {
    uint32 jobCount = 0;
    uint32 nextJob = 0;
    uint32 failed = 0;

    bool TryClaim(uint32& job) noexcept {
        if (platform::AtomicLoad(&failed) != 0) {
            return false;
        }
        job = platform::AtomicFetchAdd(&nextJob, 1u);
        return job < jobCount;
    }

    void MarkFailed() noexcept {
        platform::AtomicStore(&failed, 1u);
    }
};

// 연속 블록을 job으로 묶는다. worker 수의 4배 정도 job을 만들되 job 하나는 MaxJobRawBytes 이하다.
uint32 BlocksPerJob(uint64 blockCount, uint32 blockSizeLog2, uint32 workers) noexcept {
    const uint64 targetJobs = (uint64{workers} + 1) * 4;
    uint64 blocks = (blockCount + targetJobs - 1) / targetJobs;
    const uint64 cap = Larger(1, MaxJobRawBytes >> blockSizeLog2);
    blocks = Smaller(Larger(blocks, 1), cap);
    return static_cast<uint32>(blocks);
}

// shared.Participate를 helper 작업들과 호출 스레드에서 실행하고 모든 helper가 끝날 때까지 기다린다.
// 제출이 실패하면 그 뒤 helper를 만들지 않는다(호출 스레드가 남은 job을 처리한다).
template <typename Shared>
void RunParticipants(WorkerScheduler& scheduler, Shared& shared, uint32 helpers,
                     Allocator& scratch) noexcept {
    WorkerTaskHandle handles[MaxHelpers];
    uint32 submitted = 0;
    while (submitted < helpers) {
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            scratch,
            [&shared](WorkerTaskContext& context) noexcept { shared.Participate(&context); });
        if (!body) {
            break;
        }
        auto handle = scheduler.Submit(Move(body.Value()));
        if (!handle) {
            break;
        }
        handles[submitted++] = Move(handle.Value());
    }

    shared.Participate(nullptr);

    // body가 shared(호출 스레드의 stack)를 참조하므로 모든 helper가 terminal이 된 뒤 반환한다. 아직
    // worker가 꺼내지 않은 helper는 회수한다(worker가 바쁘거나 이 스레드가 쥔 lock을 기다려도
    // 끝난다). 실행 중인 helper만 기다리며, 그 helper는 가져간 job 하나(16MiB 이하)를 마치면
    // 반환한다. 호출 스레드가 worker가 아님을 미리 확인했으므로 Wait는 실패하지 않는다.
    for (uint32 index = 0; index < submitted; ++index) {
        auto revoked = handles[index].TryRevoke();
        ROCKS_ASSERT(revoked.IsOk());
        if (!revoked.Value()) {
            auto waited = handles[index].Wait(platform::InfiniteTimeout);
            ROCKS_ASSERT(waited.IsOk() && waited.Value() == platform::WaitResult::Signaled);
        }
    }
}

// helper 수다. worker 수·남은 job 수·최대치와 원본 크기(helper 하나에 ParticipantMinimumBytes
// 이상)로 제한한다.
uint32 HelperCount(WorkerScheduler& scheduler, uint32 jobCount, uint64 rawBytes) noexcept {
    uint64 helpers = scheduler.Stats().workerCount;
    helpers = Smaller(helpers, jobCount - 1);
    helpers = Smaller(helpers, MaxHelpers);
    helpers = Smaller(helpers, rawBytes / ParticipantMinimumBytes);
    return static_cast<uint32>(helpers);
}

// --- 병렬 압축 ---

struct CompressJob {
    Array<uint8> payload;
    ErrorCode error = ErrorCode::None;

    explicit CompressJob(Allocator& allocator) noexcept : payload(allocator) {
    }
};

struct CompressShared {
    JobQueue queue;
    CompressionFormat format;
    int32 level;
    uint32 blockSizeLog2;
    uint32 blockCount;
    uint32 blocksPerJob;
    Span<const uint8> source;
    uint8* table;
    CompressJob* jobs;
    Allocator* scratch;
    // 사전 블록이면 그 사전이다(호출 동안 settings가 handle을 가진다).
    const SharedPtr<detail::DictionaryState>* dictionary;

    ErrorCode RunJob(uint32 job, detail::CompressorState*& compressor) noexcept {
        Array<uint8>& payload = jobs[job].payload;
        const uint32 first = job * blocksPerJob;
        const uint32 end = blockCount - first < blocksPerJob ? blockCount : first + blocksPerJob;
        // job 결과는 원본보다 커지지 않으므로 한 번에 확보한다(블록마다 재할당하지 않는다).
        const usize jobRawBytes =
            static_cast<usize>((end == blockCount ? source.Size() : usize{end} << blockSizeLog2) -
                               (usize{first} << blockSizeLog2));
        auto reserved = payload.TryReserve(jobRawBytes);
        if (!reserved) {
            return reserved.Error();
        }
        for (uint32 block = first; block < end; ++block) {
            const usize rawBytes = BlockRawBytes(source.Size(), blockSizeLog2, blockCount, block);
            const Span<const uint8> raw(source.Data() + (usize{block} << blockSizeLog2), rawBytes);
            const usize base = payload.Size();
            auto grown = payload.TryAppendUninitialized(rawBytes);
            if (!grown) {
                return grown.Error();
            }
            auto stored =
                EncodeBlock(compressor, format, level, *scratch, raw, grown.Value(), *dictionary);
            if (!stored) {
                return stored.Error();
            }
            detail::RestoreLength(payload, base + stored.Value());
            StoreEntry(
                table, block, stored.Value(),
                detail::zlib::Crc32(Span<const uint8>(payload.Data() + base, stored.Value())));
        }
        return ErrorCode::None;
    }

    void Participate(WorkerTaskContext* context) noexcept {
        detail::CompressorState* compressor = nullptr;
        // helper는 job을 가져가기 전에 문맥을 만든다. 만들지 못하면 참여하지 않는다(호출 스레드가
        // 남은 job을 처리하므로 helper 자원 부족이 호출 전체를 실패시키지 않는다).
        if (context != nullptr && Compresses(format, level)) {
            auto created =
                detail::CreateCompressorState(format, level, *scratch, false, *dictionary);
            if (!created) {
                return;
            }
            compressor = created.Value();
        }
        uint32 job = 0;
        // worker는 취소 요청을 받으면 손을 뗀다. 호출 스레드가 남은 job을 처리한다.
        while ((context == nullptr || !context->IsCancellationRequested()) && queue.TryClaim(job)) {
            const ErrorCode error = RunJob(job, compressor);
            if (error != ErrorCode::None) {
                jobs[job].error = error;
                queue.MarkFailed();
            }
        }
        detail::DestroyCompressorState(compressor);
    }
};

} // namespace

// --- 설정 ---

struct CompressedBlocksSettingsAccess {
    static const SharedPtr<detail::DictionaryState>&
    Dictionary(const CompressedBlocksSettings& settings) noexcept {
        return settings.dictionary_;
    }

    static CompressedBlocksSettings
    Make(const CompressionSettings& compression, uint32 blockSizeLog2,
         const SharedPtr<detail::DictionaryState>& dictionary) noexcept {
        return CompressedBlocksSettings(compression, blockSizeLog2, dictionary);
    }
};

Result<CompressedBlocksSettings>
CompressedBlocksSettings::TryMake(const CompressionSettings& compression,
                                  uint32 blockSizeLog2) noexcept {
    if (!IsBlockFormat(compression.Format()) || !IsBlockSizeLog2(blockSizeLog2)) {
        return Result<CompressedBlocksSettings>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<CompressedBlocksSettings>::Success(CompressedBlocksSettingsAccess::Make(
        compression, blockSizeLog2, SharedPtr<detail::DictionaryState>()));
}

Result<CompressedBlocksSettings>
CompressedBlocksSettings::TryMake(const CompressDictionary& dictionary,
                                  uint32 blockSizeLog2) noexcept {
    const auto& state = detail::DictionaryAccess::State(dictionary);
    if (!state) {
        return Result<CompressedBlocksSettings>::Failure(ErrorCode::StaleHandle);
    }
    if (!IsBlockFormat(state->format) || !IsBlockSizeLog2(blockSizeLog2)) {
        return Result<CompressedBlocksSettings>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<CompressedBlocksSettings>::Success(
        CompressedBlocksSettingsAccess::Make(dictionary.Settings(), blockSizeLog2, state));
}

Result<usize> TryGetCompressedBlocksBound(uint32 blockSizeLog2, uint64 rawSize) noexcept {
    if (!IsBlockSizeLog2(blockSizeLog2)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 count = BlockCountFor(rawSize, blockSizeLog2);
    if (count > MaxBlockCount) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 index = IndexBytesFor(static_cast<uint32>(count));
    if (rawSize > static_cast<uint64>(static_cast<usize>(-1)) - index) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(static_cast<usize>(index + rawSize));
}

// --- writer ---

struct CompressedBlocksWriter::State {
    Allocator* allocator = nullptr;
    // 처음 압축할 블록에서 만든다(빈 원본·Deflate 수준 0은 만들지 않는다).
    detail::CompressorState* compressor = nullptr;
    uint8* table = nullptr;
    CompressionFormat format = CompressionFormat::Zstd;
    int32 level = 0;
    uint32 blockSizeLog2 = 0;
    uint64 rawSize = 0;
    uint32 blockCount = 0;
    uint32 nextBlock = 0;
    uint64 payloadSize = 0;
    SharedPtr<detail::DictionaryState> dictionary;
};

CompressedBlocksWriter::~CompressedBlocksWriter() noexcept {
    if (state_ == nullptr) {
        return;
    }

    // 압축 문맥이 사전을 참조하므로 문맥을 먼저 정리한 뒤 상태(사전 handle)를 소멸한다.
    Allocator& allocator = *state_->allocator;
    detail::DestroyCompressorState(state_->compressor);
    detail::CodecFree(allocator, state_->table);
    DestroyAt(state_);
    detail::CodecFree(allocator, state_);
}

CompressedBlocksWriter::CompressedBlocksWriter(CompressedBlocksWriter&& other) noexcept
    : state_(other.state_) {
    other.state_ = nullptr;
}

CompressedBlocksWriter& CompressedBlocksWriter::operator=(CompressedBlocksWriter&& other) noexcept {
    if (this != &other) {
        CompressedBlocksWriter released(Move(*this));
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

Result<CompressedBlocksWriter>
CompressedBlocksWriter::TryBegin(const CompressedBlocksSettings& settings, uint64 rawSize,
                                 Allocator& allocator) noexcept {
    const uint64 count = BlockCountFor(rawSize, settings.BlockSizeLog2());
    if (count > MaxBlockCount) {
        return Result<CompressedBlocksWriter>::Failure(ErrorCode::InvalidArgument);
    }

    void* memory = detail::CodecAllocate(allocator, sizeof(State));
    if (memory == nullptr) {
        return Result<CompressedBlocksWriter>::Failure(ErrorCode::OutOfMemory);
    }
    State* state = ConstructAt<State>(memory);
    state->allocator = &allocator;
    state->format = settings.Compression().Format();
    state->level = settings.Compression().Level();
    state->blockSizeLog2 = settings.BlockSizeLog2();
    state->dictionary = CompressedBlocksSettingsAccess::Dictionary(settings);
    state->rawSize = rawSize;
    state->blockCount = static_cast<uint32>(count);
    CompressedBlocksWriter writer(state);

    if (count != 0) {
        state->table = static_cast<uint8*>(detail::CodecAllocate(allocator, EntryBytes * count));
        if (state->table == nullptr) {
            return Result<CompressedBlocksWriter>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<CompressedBlocksWriter>::Success(Move(writer));
}

usize CompressedBlocksWriter::IndexBytes() const noexcept {
    return state_ != nullptr ? IndexBytesFor(state_->blockCount) : 0;
}

uint32 CompressedBlocksWriter::BlockCount() const noexcept {
    return state_ != nullptr ? state_->blockCount : 0;
}

Result<usize> CompressedBlocksWriter::TryAppendBlock(Span<const uint8> rawBlock,
                                                     Array<uint8>& payloadOutput) noexcept {
    if (state_ == nullptr) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    State& state = *state_;
    if (state.nextBlock == state.blockCount ||
        rawBlock.Size() !=
            BlockRawBytes(state.rawSize, state.blockSizeLog2, state.blockCount, state.nextBlock) ||
        detail::OverlapsArrayStorage(rawBlock, payloadOutput)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize base = payloadOutput.Size();
    auto grown = payloadOutput.TryAppendUninitialized(rawBlock.Size());
    if (!grown) {
        return Result<usize>::Failure(grown.Error());
    }

    auto stored = EncodeBlock(state.compressor, state.format, state.level, *state.allocator,
                              rawBlock, grown.Value(), state.dictionary);
    if (!stored) {
        detail::RestoreLength(payloadOutput, base);
        return stored;
    }

    detail::RestoreLength(payloadOutput, base + stored.Value());
    const uint32 crc =
        detail::zlib::Crc32(Span<const uint8>(payloadOutput.Data() + base, stored.Value()));
    StoreEntry(state.table, state.nextBlock, stored.Value(), crc);
    ++state.nextBlock;
    state.payloadSize += stored.Value();
    return stored;
}

Result<void> CompressedBlocksWriter::TryFinishIndex(Span<uint8> indexOut) const noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const State& state = *state_;
    if (state.nextBlock != state.blockCount || indexOut.Size() != IndexBytesFor(state.blockCount)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    WriteIndex(indexOut.Data(), state.format, state.blockSizeLog2, state.rawSize, state.blockCount,
               state.payloadSize, state.table, state.dictionary.Get());
    return Result<void>::Success();
}

// --- 압축 ---

Result<usize> TryCompressBlocksAppend(const CompressedBlocksSettings& settings,
                                      Span<const uint8> source, Array<uint8>& output,
                                      Allocator& scratch) noexcept {
    if (detail::OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto writer = CompressedBlocksWriter::TryBegin(settings, source.Size(), scratch);
    if (!writer) {
        return Result<usize>::Failure(writer.Error());
    }

    // 인덱스 자리를 먼저 잡고 payload를 붙인 뒤 인덱스를 채운다.
    const usize base = output.Size();
    const usize indexBytes = writer.Value().IndexBytes();
    auto reserved = output.TryAppendUninitialized(indexBytes);
    if (!reserved) {
        return Result<usize>::Failure(reserved.Error());
    }

    const usize blockSize = settings.BlockSize();
    for (usize offset = 0; offset < source.Size(); offset += blockSize) {
        const usize rawBytes =
            source.Size() - offset < blockSize ? source.Size() - offset : blockSize;
        auto appended = writer.Value().TryAppendBlock(
            Span<const uint8>(source.Data() + offset, rawBytes), output);
        if (!appended) {
            detail::RestoreLength(output, base);
            return appended;
        }
    }

    auto finished = writer.Value().TryFinishIndex(Span<uint8>(output.Data() + base, indexBytes));
    ROCKS_ASSERT(finished.IsOk());
    return Result<usize>::Success(output.Size() - base);
}

Result<usize> TryCompressBlocksAppend(WorkerScheduler& scheduler,
                                      const CompressedBlocksSettings& settings,
                                      Span<const uint8> source, Array<uint8>& output,
                                      Allocator& scratch) noexcept {
    if (scheduler.IsCurrentWorker()) {
        return Result<usize>::Failure(ErrorCode::WrongThread);
    }

    const uint32 blockSizeLog2 = settings.BlockSizeLog2();
    const uint64 count = BlockCountFor(source.Size(), blockSizeLog2);
    if (count <= SequentialBlockLimit || source.Size() < ParallelMinimumBytes) {
        return TryCompressBlocksAppend(settings, source, output, scratch);
    }
    if (count > MaxBlockCount) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    if (detail::OverlapsArrayStorage(source, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 blockCount = static_cast<uint32>(count);
    const uint32 workers = scheduler.Stats().workerCount;
    const uint32 blocksPerJob = BlocksPerJob(blockCount, blockSizeLog2, workers);
    const uint32 jobCount = (blockCount + blocksPerJob - 1) / blocksPerJob;

    auto* table = static_cast<uint8*>(detail::CodecAllocate(scratch, EntryBytes * blockCount));
    auto* jobs =
        static_cast<CompressJob*>(detail::CodecAllocate(scratch, sizeof(CompressJob) * jobCount));
    if (table == nullptr || jobs == nullptr) {
        detail::CodecFree(scratch, table);
        detail::CodecFree(scratch, jobs);
        return Result<usize>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32 job = 0; job < jobCount; ++job) {
        ConstructAt<CompressJob>(jobs + job, scratch);
    }

    CompressShared shared{};
    shared.queue.jobCount = jobCount;
    shared.format = settings.Compression().Format();
    shared.level = settings.Compression().Level();
    shared.blockSizeLog2 = blockSizeLog2;
    shared.blockCount = blockCount;
    shared.blocksPerJob = blocksPerJob;
    shared.source = source;
    shared.table = table;
    shared.jobs = jobs;
    shared.scratch = &scratch;
    shared.dictionary = &CompressedBlocksSettingsAccess::Dictionary(settings);
    RunParticipants(scheduler, shared, HelperCount(scheduler, jobCount, source.Size()), scratch);

    // 가장 앞 job의 오류를 돌려준다. 성공이면 output을 한 번만 늘려 인덱스와 payload를 복사한다.
    ErrorCode error = ErrorCode::None;
    uint64 payloadSize = 0;
    for (uint32 job = 0; job < jobCount && error == ErrorCode::None; ++job) {
        error = jobs[job].error;
        payloadSize += jobs[job].payload.Size();
    }

    usize written = 0;
    if (error == ErrorCode::None) {
        const usize indexBytes = IndexBytesFor(blockCount);
        auto grown = output.TryAppendUninitialized(indexBytes + static_cast<usize>(payloadSize));
        if (!grown) {
            error = grown.Error();
        } else {
            uint8* cursor = grown.Value().Data();
            WriteIndex(cursor, shared.format, blockSizeLog2, source.Size(), blockCount, payloadSize,
                       table, shared.dictionary->Get());
            cursor += indexBytes;
            for (uint32 job = 0; job < jobCount; ++job) {
                platform::MemoryCopy(cursor, jobs[job].payload.Data(), jobs[job].payload.Size());
                cursor += jobs[job].payload.Size();
            }
            written = grown.Value().Size();
        }
    }

    for (uint32 job = 0; job < jobCount; ++job) {
        DestroyAt(jobs + job);
    }
    detail::CodecFree(scratch, jobs);
    detail::CodecFree(scratch, table);
    return error == ErrorCode::None ? Result<usize>::Success(written)
                                    : Result<usize>::Failure(error);
}

// --- reader ---

struct CompressedBlocksAccess {
    using Entry = CompressedBlocksReader::Entry;

    // stored가 블록 저장 byte를 덮는지 확인하고 그 구간을 돌려준다.
    static Result<Span<const uint8>> StoredBytes(const Entry& entry, Span<const uint8> stored,
                                                 uint64 storedBase) noexcept {
        if (entry.storedOffset < storedBase || entry.storedOffset - storedBase > stored.Size() ||
            entry.storedSize > stored.Size() - (entry.storedOffset - storedBase)) {
            return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Span<const uint8>>::Success(
            Span<const uint8>(stored.Data() + (entry.storedOffset - storedBase), entry.storedSize));
    }

    // 블록 하나를 out(정확히 블록 원본 크기)에 해제한다. dictionary는 reader의 사전이다. 해제하는
    // 동안만 문맥에 걸고 끝나면 뗀다(문맥은 사전과 무관하게 여러 reader에 쓸 수 있고, 해제된 사전을
    // 가리킨 채 남지 않는다).
    static Result<void> DecodeBlock(const Entry& entry, Span<const uint8> stored, uint64 storedBase,
                                    bool verify, Decompressor& decompressor, Span<uint8> out,
                                    const detail::DictionaryState* dictionary) noexcept {
        auto bytes = StoredBytes(entry, stored, storedBase);
        if (!bytes) {
            return Result<void>::Failure(bytes.Error());
        }
        if (verify && detail::zlib::Crc32(bytes.Value()) != entry.storedCrc) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (entry.storedSize == out.Size()) {
            platform::MemoryCopy(out.Data(), bytes.Value().Data(), out.Size());
            return Result<void>::Success();
        }

        // 블록 원본 크기는 표가 보장하므로 코덱의 출력 부족도 손상이다. 사전은 헤더 지문으로 이미
        // 맞췄으므로 사전 ID가 다르거나 없는 블록·사전 없는 컨테이너의 사전 블록(Unsupported)도
        // 형식 위반이다.
        detail::DecompressorState& state = *detail::DecompressorAccess::State(decompressor);
        auto bound = detail::BindDecompressorDictionary(state, dictionary);
        if (!bound) {
            return bound;
        }
        auto decoded = decompressor.TryDecompress(bytes.Value(), out);
        if (dictionary != nullptr) {
            auto unbound = detail::BindDecompressorDictionary(state, nullptr);
            ROCKS_ASSERT(unbound.IsOk());
        }
        if (!decoded) {
            const ErrorCode error = decoded.Error();
            return Result<void>::Failure(error == ErrorCode::CapacityExceeded ||
                                                 error == ErrorCode::Unsupported
                                             ? ErrorCode::InvalidArgument
                                             : error);
        }
        return decoded.Value() == out.Size() ? Result<void>::Success()
                                             : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 읽기 범위 검사다. 0byte는 호출자가 먼저 처리한다.
    static bool InRange(const CompressedBlocksReader& reader, uint64 rawOffset,
                        uint64 rawBytes) noexcept {
        return rawBytes <= reader.rawSize_ && rawOffset <= reader.rawSize_ - rawBytes;
    }
};

namespace {

// --- 병렬 읽기 ---

struct ReadShared {
    JobQueue queue;
    const CompressedBlocksReader* reader;
    const CompressedBlocksAccess::Entry* entries;
    CompressionFormat format;
    uint32 blockSizeLog2;
    uint32 firstBlock;
    uint32 lastBlock;
    uint32 blocksPerJob;
    uint64 rawOffset;
    Span<uint8> destination;
    Span<const uint8> stored;
    uint64 storedBase;
    bool verify;
    ErrorCode* errors;
    // reader의 사전이다(없으면 nullptr). 해제할 때마다 helper 문맥에 걸고 뗀다.
    const detail::DictionaryState* dictionary;
    Allocator* scratch;

    struct Worker {
        Decompressor decompressor;
        uint8* buffer = nullptr;
        bool created = false;
    };

    ErrorCode RunJob(uint32 job, Worker& worker) noexcept {
        if (!worker.created) {
            auto created = Decompressor::TryCreate(format, *scratch);
            if (!created) {
                return created.Error();
            }
            worker.decompressor = Move(created.Value());
            worker.created = true;
        }

        const uint64 rawSize = reader->RawSize();
        const uint32 count = reader->BlockCount();
        const uint64 rawEnd = rawOffset + destination.Size();
        const uint32 first = firstBlock + job * blocksPerJob;
        const uint32 end = lastBlock - first < blocksPerJob ? lastBlock + 1 : first + blocksPerJob;
        for (uint32 block = first; block < end; ++block) {
            const uint64 blockStart = uint64{block} << blockSizeLog2;
            const usize rawBytes = BlockRawBytes(rawSize, blockSizeLog2, count, block);
            const uint64 from = Larger(rawOffset, blockStart);
            const uint64 to = Smaller(rawEnd, blockStart + rawBytes);
            const Span<uint8> slice(destination.Data() + (from - rawOffset),
                                    static_cast<usize>(to - from));
            if (from == blockStart && to == blockStart + rawBytes) {
                auto decoded =
                    CompressedBlocksAccess::DecodeBlock(entries[block], stored, storedBase, verify,
                                                        worker.decompressor, slice, dictionary);
                if (!decoded) {
                    return decoded.Error();
                }
                continue;
            }

            // 읽기 범위 양 끝에서 블록 일부만 걸친다. 임시 버퍼로 해제해 필요한 부분만 복사한다.
            if (worker.buffer == nullptr) {
                worker.buffer =
                    static_cast<uint8*>(detail::CodecAllocate(*scratch, usize{1} << blockSizeLog2));
                if (worker.buffer == nullptr) {
                    return ErrorCode::OutOfMemory;
                }
            }
            auto decoded = CompressedBlocksAccess::DecodeBlock(
                entries[block], stored, storedBase, verify, worker.decompressor,
                Span<uint8>(worker.buffer, rawBytes), dictionary);
            if (!decoded) {
                return decoded.Error();
            }
            platform::MemoryCopy(slice.Data(), worker.buffer + (from - blockStart), slice.Size());
        }
        return ErrorCode::None;
    }

    void Participate(WorkerTaskContext* context) noexcept {
        Worker worker;
        // helper는 job을 가져가기 전에 해제 문맥을 만든다. 만들지 못하면 참여하지 않는다.
        if (context != nullptr) {
            auto created = Decompressor::TryCreate(format, *scratch);
            if (!created) {
                return;
            }
            worker.decompressor = Move(created.Value());
            worker.created = true;
        }
        uint32 job = 0;
        while ((context == nullptr || !context->IsCancellationRequested()) && queue.TryClaim(job)) {
            const ErrorCode error = RunJob(job, worker);
            if (error != ErrorCode::None) {
                errors[job] = error;
                queue.MarkFailed();
            }
        }
        detail::CodecFree(*scratch, worker.buffer);
    }
};

} // namespace

CompressedBlocksReader::~CompressedBlocksReader() noexcept {
    Release();
}

CompressedBlocksReader::CompressedBlocksReader(CompressedBlocksReader&& other) noexcept
    : allocator_(other.allocator_), entries_(other.entries_), rawSize_(other.rawSize_),
      containerBytes_(other.containerBytes_), indexBytes_(other.indexBytes_),
      blockCount_(other.blockCount_), blockSizeLog2_(other.blockSizeLog2_), format_(other.format_),
      id_(other.id_), dictionary_(Move(other.dictionary_)),
      dictionaryFingerprint_(other.dictionaryFingerprint_) {
    other.allocator_ = nullptr;
    other.entries_ = nullptr;
    other.dictionaryFingerprint_ = 0;
}

CompressedBlocksReader& CompressedBlocksReader::operator=(CompressedBlocksReader&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        entries_ = other.entries_;
        rawSize_ = other.rawSize_;
        containerBytes_ = other.containerBytes_;
        indexBytes_ = other.indexBytes_;
        blockCount_ = other.blockCount_;
        blockSizeLog2_ = other.blockSizeLog2_;
        format_ = other.format_;
        id_ = other.id_;
        dictionary_ = Move(other.dictionary_);
        dictionaryFingerprint_ = other.dictionaryFingerprint_;
        other.allocator_ = nullptr;
        other.entries_ = nullptr;
        other.dictionaryFingerprint_ = 0;
    }
    return *this;
}

void CompressedBlocksReader::Release() noexcept {
    if (allocator_ != nullptr) {
        detail::CodecFree(*allocator_, entries_);
    }
    allocator_ = nullptr;
    entries_ = nullptr;
    dictionary_.Reset();
    dictionaryFingerprint_ = 0;
}

Result<usize> CompressedBlocksReader::TryGetIndexBytes(Span<const uint8> header,
                                                       uint64 containerBytes) noexcept {
    auto parsed = ParseHeader(header, containerBytes);
    if (!parsed) {
        return Result<usize>::Failure(parsed.Error());
    }

    return Result<usize>::Success(IndexBytesFor(parsed.Value().blockCount));
}

Result<CompressedBlocksReader> CompressedBlocksReader::TryOpen(Span<const uint8> index,
                                                               uint64 containerBytes,
                                                               Allocator& allocator) noexcept {
    return TryOpen(index, containerBytes, Span<const DecompressDictionary>(), allocator);
}

Result<CompressedBlocksReader>
CompressedBlocksReader::TryOpen(Span<const uint8> index, uint64 containerBytes,
                                Span<const DecompressDictionary> dictionaries,
                                Allocator& allocator) noexcept {
    for (const DecompressDictionary& candidate : dictionaries) {
        if (!candidate.IsValid()) {
            return Result<CompressedBlocksReader>::Failure(ErrorCode::StaleHandle);
        }
    }

    auto parsed = ParseHeader(index, containerBytes);
    if (!parsed) {
        return Result<CompressedBlocksReader>::Failure(parsed.Error());
    }

    const Header& header = parsed.Value();
    const usize indexBytes = IndexBytesFor(header.blockCount);
    if (index.Size() < indexBytes) {
        return Result<CompressedBlocksReader>::Failure(ErrorCode::InvalidArgument);
    }

    const uint8* table = index.Data() + CompressedBlocksHeaderBytes;
    const usize tableBytes = EntryBytes * header.blockCount;
    uint32 crc = detail::zlib::Crc32(Span<const uint8>(index.Data(), CrcCoveredHeaderBytes));
    crc = detail::zlib::Crc32(Span<const uint8>(table, tableBytes), crc);
    if (crc != header.indexCrc) {
        return Result<CompressedBlocksReader>::Failure(ErrorCode::InvalidArgument);
    }

    // CRC 뒤에 사전을 고른다(지문 byte 손상은 CRC가 InvalidArgument로 거른다). 위치 표 할당 전이다.
    // 지문이 같은데 내용(Digest)이 다른 후보가 둘 이상이면 어느 사전인지 정할 수 없어
    // InvalidArgument다. 64bit 지문은 의도적으로 맞출 수 있고, raw 사전은 frame에 ID가 없어 잘못
    // 고르면 검사가 없다.
    SharedPtr<detail::DictionaryState> dictionary;
    if (header.dictionary) {
        for (const DecompressDictionary& candidate : dictionaries) {
            const auto& state = detail::DictionaryAccess::State(candidate);
            if (state->format != header.format || FingerprintOf(*state) != header.fingerprint) {
                continue;
            }
            if (!dictionary) {
                dictionary = state;
            } else if (dictionary->digest != state->digest) {
                return Result<CompressedBlocksReader>::Failure(ErrorCode::InvalidArgument);
            }
        }
        if (!dictionary) {
            return Result<CompressedBlocksReader>::Failure(ErrorCode::Unsupported);
        }
    }

    CompressedBlocksReader reader;
    reader.dictionary_ = Move(dictionary);
    reader.dictionaryFingerprint_ = header.fingerprint;
    reader.allocator_ = &allocator;
    reader.rawSize_ = header.rawSize;
    reader.containerBytes_ = containerBytes;
    reader.indexBytes_ = indexBytes;
    reader.blockCount_ = header.blockCount;
    reader.blockSizeLog2_ = header.blockSizeLog2;
    reader.format_ = header.format;
    reader.id_ = platform::AtomicFetchAdd(&ReaderSerial, uint64{1}) + 1;
    if (header.blockCount == 0) {
        return Result<CompressedBlocksReader>::Success(Move(reader));
    }

    reader.entries_ =
        static_cast<Entry*>(detail::CodecAllocate(allocator, sizeof(Entry) * header.blockCount));
    if (reader.entries_ == nullptr) {
        return Result<CompressedBlocksReader>::Failure(ErrorCode::OutOfMemory);
    }

    // 저장 크기 정규형: 1 ≤ storedSize ≤ 블록 원본 크기(같으면 raw). 합은 payloadSize다.
    uint64 offset = indexBytes;
    for (uint32 block = 0; block < header.blockCount; ++block) {
        const uint32 storedSize = LoadLittleEndian<uint32>(table + EntryBytes * block);
        const uint32 storedCrc = LoadLittleEndian<uint32>(table + EntryBytes * block + 4);
        const usize rawBytes =
            BlockRawBytes(header.rawSize, header.blockSizeLog2, header.blockCount, block);
        if (storedSize == 0 || storedSize > rawBytes) {
            return Result<CompressedBlocksReader>::Failure(ErrorCode::InvalidArgument);
        }
        reader.entries_[block] = Entry{offset, storedSize, storedCrc};
        offset += storedSize;
    }
    if (offset - indexBytes != header.payloadSize) {
        return Result<CompressedBlocksReader>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<CompressedBlocksReader>::Success(Move(reader));
}

Result<CompressedBlocksReader> CompressedBlocksReader::TryOpen(Span<const uint8> container,
                                                               Allocator& allocator) noexcept {
    return TryOpen(container, container.Size(), Span<const DecompressDictionary>(), allocator);
}

Result<CompressedBlocksReader>
CompressedBlocksReader::TryOpen(Span<const uint8> container,
                                Span<const DecompressDictionary> dictionaries,
                                Allocator& allocator) noexcept {
    return TryOpen(container, container.Size(), dictionaries, allocator);
}

Result<CompressedBlocksLocation> CompressedBlocksReader::TryLocate(uint64 rawOffset,
                                                                   uint64 rawBytes) const noexcept {
    if (!IsOpen()) {
        return Result<CompressedBlocksLocation>::Failure(ErrorCode::StaleHandle);
    }
    if (!CompressedBlocksAccess::InRange(*this, rawOffset, rawBytes)) {
        return Result<CompressedBlocksLocation>::Failure(ErrorCode::InvalidArgument);
    }

    CompressedBlocksLocation location;
    if (rawBytes == 0) {
        return Result<CompressedBlocksLocation>::Success(location);
    }

    const uint32 first = static_cast<uint32>(rawOffset >> blockSizeLog2_);
    const uint32 last = static_cast<uint32>((rawOffset + rawBytes - 1) >> blockSizeLog2_);
    location.firstBlock = first;
    location.blockCount = last - first + 1;
    location.storedOffset = entries_[first].storedOffset;
    location.storedBytes =
        entries_[last].storedOffset + entries_[last].storedSize - location.storedOffset;
    return Result<CompressedBlocksLocation>::Success(location);
}

Result<void> CompressedBlocksReader::TryRead(uint64 rawOffset, Span<uint8> destination,
                                             Span<const uint8> stored, uint64 storedBase,
                                             CompressedBlocksReadContext& context,
                                             bool verify) const noexcept {
    if (!IsOpen() || context.allocator_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (context.format_ != format_ || context.blockSizeLog2_ < blockSizeLog2_ ||
        !CompressedBlocksAccess::InRange(*this, rawOffset, destination.Size()) ||
        detail::Overlaps(destination.Data(), destination.Size(), stored.Data(), stored.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (destination.Size() == 0) {
        return Result<void>::Success();
    }

    const uint64 rawEnd = rawOffset + destination.Size();
    const uint32 first = static_cast<uint32>(rawOffset >> blockSizeLog2_);
    const uint32 last = static_cast<uint32>((rawEnd - 1) >> blockSizeLog2_);
    for (uint32 block = first; block <= last; ++block) {
        const Entry& entry = entries_[block];
        const uint64 blockStart = uint64{block} << blockSizeLog2_;
        const usize rawBytes = BlockRawBytes(rawSize_, blockSizeLog2_, blockCount_, block);
        const uint64 from = Larger(rawOffset, blockStart);
        const uint64 to = Smaller(rawEnd, blockStart + rawBytes);
        const Span<uint8> slice(destination.Data() + (from - rawOffset),
                                static_cast<usize>(to - from));
        if (from == blockStart && to == blockStart + rawBytes) {
            auto decoded = CompressedBlocksAccess::DecodeBlock(
                entry, stored, storedBase, verify, context.decompressor_, slice, dictionary_.Get());
            if (!decoded) {
                return decoded;
            }
            continue;
        }

        // 블록 일부만 걸친다. 문맥 버퍼에 해제(이미 있으면 재사용)해 필요한 부분만 복사한다. 캐시를
        // 써도 stored가 블록을 덮는지와(verify면) CRC는 확인한다. 결과가 캐시 상태에 따라 달라지지
        // 않는다.
        auto bytes = CompressedBlocksAccess::StoredBytes(entry, stored, storedBase);
        if (!bytes) {
            return Result<void>::Failure(bytes.Error());
        }
        const bool hit =
            context.cached_ && context.cachedReader_ == id_ && context.cachedBlock_ == block;
        if (hit && verify && !context.cachedVerified_) {
            if (detail::zlib::Crc32(bytes.Value()) != entry.storedCrc) {
                context.cached_ = false;
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            context.cachedVerified_ = true;
        }
        if (!hit) {
            context.cached_ = false;
            auto decoded = CompressedBlocksAccess::DecodeBlock(
                entry, stored, storedBase, verify, context.decompressor_,
                Span<uint8>(context.buffer_, rawBytes), dictionary_.Get());
            if (!decoded) {
                return decoded;
            }
            context.cached_ = true;
            context.cachedVerified_ = verify;
            context.cachedReader_ = id_;
            context.cachedBlock_ = block;
        }
        platform::MemoryCopy(slice.Data(), context.buffer_ + (from - blockStart), slice.Size());
    }
    return Result<void>::Success();
}

Result<void> CompressedBlocksReader::TryRead(uint64 rawOffset, Span<uint8> destination,
                                             Span<const uint8> container,
                                             CompressedBlocksReadContext& context,
                                             bool verify) const noexcept {
    return TryRead(rawOffset, destination, container, 0, context, verify);
}

Result<void> CompressedBlocksReader::TryRead(WorkerScheduler& scheduler, uint64 rawOffset,
                                             Span<uint8> destination, Span<const uint8> stored,
                                             uint64 storedBase, Allocator& scratch,
                                             bool verify) const noexcept {
    if (scheduler.IsCurrentWorker()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (!IsOpen()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (!CompressedBlocksAccess::InRange(*this, rawOffset, destination.Size()) ||
        detail::Overlaps(destination.Data(), destination.Size(), stored.Data(), stored.Size())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (destination.Size() == 0) {
        return Result<void>::Success();
    }

    const uint32 first = static_cast<uint32>(rawOffset >> blockSizeLog2_);
    const uint32 last = static_cast<uint32>((rawOffset + destination.Size() - 1) >> blockSizeLog2_);
    const uint32 blocks = last - first + 1;
    const uint32 workers = scheduler.Stats().workerCount;
    const bool sequential =
        blocks <= SequentialBlockLimit || destination.Size() < ParallelMinimumBytes;
    const uint32 blocksPerJob = sequential ? blocks : BlocksPerJob(blocks, blockSizeLog2_, workers);
    const uint32 jobCount = (blocks + blocksPerJob - 1) / blocksPerJob;
    auto* errors =
        static_cast<ErrorCode*>(detail::CodecAllocate(scratch, sizeof(ErrorCode) * jobCount));
    if (errors == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    for (uint32 job = 0; job < jobCount; ++job) {
        errors[job] = ErrorCode::None;
    }

    ReadShared shared{};
    shared.queue.jobCount = jobCount;
    shared.reader = this;
    shared.entries = entries_;
    shared.format = format_;
    shared.blockSizeLog2 = blockSizeLog2_;
    shared.firstBlock = first;
    shared.lastBlock = last;
    shared.blocksPerJob = blocksPerJob;
    shared.rawOffset = rawOffset;
    shared.destination = destination;
    shared.stored = stored;
    shared.storedBase = storedBase;
    shared.verify = verify;
    shared.errors = errors;
    shared.dictionary = dictionary_.Get();
    shared.scratch = &scratch;
    RunParticipants(scheduler, shared,
                    jobCount > 1 ? HelperCount(scheduler, jobCount, destination.Size()) : 0,
                    scratch);

    ErrorCode error = ErrorCode::None;
    for (uint32 job = 0; job < jobCount && error == ErrorCode::None; ++job) {
        error = errors[job];
    }
    detail::CodecFree(scratch, errors);
    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

Result<void> CompressedBlocksReader::TryVerify(Span<const uint8> stored,
                                               uint64 storedBase) const noexcept {
    if (!IsOpen()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    for (uint32 block = 0; block < blockCount_; ++block) {
        auto bytes = CompressedBlocksAccess::StoredBytes(entries_[block], stored, storedBase);
        if (!bytes || detail::zlib::Crc32(bytes.Value()) != entries_[block].storedCrc) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }
    return Result<void>::Success();
}

// --- 읽기 문맥 ---

CompressedBlocksReadContext::~CompressedBlocksReadContext() noexcept {
    Release();
}

CompressedBlocksReadContext::CompressedBlocksReadContext(
    CompressedBlocksReadContext&& other) noexcept
    : decompressor_(Move(other.decompressor_)), allocator_(other.allocator_),
      buffer_(other.buffer_), format_(other.format_), blockSizeLog2_(other.blockSizeLog2_),
      cached_(other.cached_), cachedVerified_(other.cachedVerified_),
      cachedReader_(other.cachedReader_), cachedBlock_(other.cachedBlock_) {
    other.allocator_ = nullptr;
    other.buffer_ = nullptr;
    other.cached_ = false;
}

CompressedBlocksReadContext&
CompressedBlocksReadContext::operator=(CompressedBlocksReadContext&& other) noexcept {
    if (this != &other) {
        Release();
        decompressor_ = Move(other.decompressor_);
        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        format_ = other.format_;
        blockSizeLog2_ = other.blockSizeLog2_;
        cached_ = other.cached_;
        cachedVerified_ = other.cachedVerified_;
        cachedReader_ = other.cachedReader_;
        cachedBlock_ = other.cachedBlock_;
        other.allocator_ = nullptr;
        other.buffer_ = nullptr;
        other.cached_ = false;
    }
    return *this;
}

void CompressedBlocksReadContext::Release() noexcept {
    if (allocator_ != nullptr) {
        detail::CodecFree(*allocator_, buffer_);
    }
    allocator_ = nullptr;
    buffer_ = nullptr;
    cached_ = false;
}

Result<CompressedBlocksReadContext>
CompressedBlocksReadContext::TryCreate(const CompressedBlocksReader& reader,
                                       Allocator& allocator) noexcept {
    if (!reader.IsOpen()) {
        return Result<CompressedBlocksReadContext>::Failure(ErrorCode::StaleHandle);
    }

    return TryCreate(reader.Format(), reader.BlockSizeLog2(), allocator);
}

Result<CompressedBlocksReadContext>
CompressedBlocksReadContext::TryCreate(CompressionFormat format, uint32 blockSizeLog2,
                                       Allocator& allocator) noexcept {
    if (!IsBlockFormat(format) || !IsBlockSizeLog2(blockSizeLog2)) {
        return Result<CompressedBlocksReadContext>::Failure(ErrorCode::InvalidArgument);
    }

    auto decompressor = Decompressor::TryCreate(format, allocator);
    if (!decompressor) {
        return Result<CompressedBlocksReadContext>::Failure(decompressor.Error());
    }

    CompressedBlocksReadContext context;
    context.decompressor_ = Move(decompressor.Value());
    context.buffer_ =
        static_cast<uint8*>(detail::CodecAllocate(allocator, usize{1} << blockSizeLog2));
    if (context.buffer_ == nullptr) {
        return Result<CompressedBlocksReadContext>::Failure(ErrorCode::OutOfMemory);
    }
    context.allocator_ = &allocator;
    context.format_ = format;
    context.blockSizeLog2_ = blockSizeLog2;
    return Result<CompressedBlocksReadContext>::Success(Move(context));
}

} // namespace rocks
