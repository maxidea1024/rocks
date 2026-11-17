#include "CompressedBlocksInternal.h"

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>

namespace rocks {

using namespace detail::blocks;

namespace {

constexpr uint8 Magic[4] = {'R', 'C', 'B', 'K'};
// 2: 헤더 48byte(사전 bit·지문). 40byte 헤더의 1은 지원하지 않는 version(Unsupported)이다.
constexpr uint16 FormatVersion = 2;
// indexCrc32가 덮는 헤더 앞부분이다(CRC 필드 앞까지).
constexpr usize CrcCoveredHeaderBytes = 44;
// flags bit0: 블록이 사전으로 압축되었다(헤더 32의 dictionaryFingerprint가 그 사전이다).
constexpr uint32 FlagDictionary = 1;

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
    case CompressionFormat::Brotli:
        // 해제 전용이라 블록을 만들 수 없다(Zlib·Lz4Frame과 같은 InvalidArgument).
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

} // namespace

namespace detail::blocks {

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

} // namespace detail::blocks

// --- 설정 ---

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
    if (rawSize > static_cast<uint64>(MaxSize) - index) {
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
    State* state = InitializeAt<State>(memory);
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

// --- reader ---

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
