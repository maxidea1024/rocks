#pragma once

#include "CompressionInternal.h"

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Memory.h>

// 블록 컨테이너(CompressedBlocks)의 내부 경계다. 순차 경로(Compression 모듈의
// CompressedBlocks.cpp)와 병렬 경로(AsyncCompression 모듈의 ParallelCompressedBlocks.cpp)가 같은
// 블록 인코딩·인덱스 쓰기· 블록 해제를 쓰도록 두 곳에서 함께 include한다. 다른 모듈은 include하지
// 않는다.
namespace rocks {

namespace detail::blocks {

inline constexpr usize EntryBytes = 8;
inline constexpr uint64 MaxBlockCount = NumericLimits<uint32>::Max();

inline uint64 BlockCountFor(uint64 rawSize, uint32 blockSizeLog2) noexcept {
    const uint64 mask = (uint64{1} << blockSizeLog2) - 1;
    return (rawSize >> blockSizeLog2) + ((rawSize & mask) != 0 ? 1 : 0);
}

// index번 블록의 원본 크기다. 마지막 블록만 짧을 수 있다.
inline usize BlockRawBytes(uint64 rawSize, uint32 blockSizeLog2, uint32 count,
                           uint32 index) noexcept {
    const uint64 blockSize = uint64{1} << blockSizeLog2;
    return static_cast<usize>(index + 1 < count ? blockSize
                                                : rawSize - uint64{count - 1} * blockSize);
}

inline usize IndexBytesFor(uint32 count) noexcept {
    return CompressedBlocksHeaderBytes + EntryBytes * count;
}

inline uint64 Smaller(uint64 first, uint64 second) noexcept {
    return first < second ? first : second;
}

inline uint64 Larger(uint64 first, uint64 second) noexcept {
    return first > second ? first : second;
}

// 이 설정으로 압축을 시도하는지다. Deflate 수준 0은 항상 raw로 저장하므로 압축 문맥이 필요 없다.
inline bool Compresses(CompressionFormat format, int32 level) noexcept {
    return !(format == CompressionFormat::Deflate && level == 0);
}

inline void StoreEntry(uint8* table, uint32 index, usize storedSize, uint32 crc) noexcept {
    StoreLittleEndian<uint32>(table + EntryBytes * index, static_cast<uint32>(storedSize));
    StoreLittleEndian<uint32>(table + EntryBytes * index + 4, crc);
}

// 헤더와 블록 표를 쓰고 CRC를 계산한다. out은 IndexBytesFor(count) byte다.
ROCKS_COMPRESSION_API void WriteIndex(uint8* out, CompressionFormat format, uint32 blockSizeLog2,
                                      uint64 rawSize, uint32 count, uint64 payloadSize,
                                      const uint8* table,
                                      const detail::DictionaryState* dictionary) noexcept;

// raw 블록 하나를 저장 형식으로 dst(용량 raw.Size())에 쓰고 저장 크기를 반환한다. 압축 결과가
// 원본보다 작지 않으면(용량 raw − 1에서 CapacityExceeded) raw로 저장한다. compressor는 압축을
// 시도할 때만 쓰며, 없으면 allocator로 만든다(만들지 못하면 그 오류다).
ROCKS_COMPRESSION_API Result<usize>
EncodeBlock(detail::CompressorState*& compressor, CompressionFormat format, int32 level,
            Allocator& allocator, Span<const uint8> raw, Span<uint8> dst,
            const SharedPtr<detail::DictionaryState>& dictionary) noexcept;

} // namespace detail::blocks

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

struct CompressedBlocksAccess {
    using Entry = CompressedBlocksReader::Entry;

    static const Entry* Entries(const CompressedBlocksReader& reader) noexcept {
        return reader.entries_;
    }

    static const detail::DictionaryState*
    Dictionary(const CompressedBlocksReader& reader) noexcept {
        return reader.dictionary_.Get();
    }

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

} // namespace rocks
