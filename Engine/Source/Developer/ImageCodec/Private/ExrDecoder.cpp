#include "ExternalAllocation.h"
#include "ImageCodecInternal.h"

#include <Core/Math/Float16.h>
#include <openexr.h>

// OpenEXR 디코더다(Docs/Design/AssetSystem 12.10절 — OpenEXRCore 3.4.3 C API, UE
// ExrImageWrapper.cpp와 같은 채널 규칙). part 0의 scanline·tile(level 0)을 읽는다. 채널 하나면 단일
// 채널 결과, 둘 이상이면 이름 마지막 글자로 R·G·B·A 칸에 넣고 남은 채널을 뒤에서부터 빈 칸에
// 채운다. part 0의 모든 채널이 half면 16F, 아니면 32F다. 받지 않는 압축(HTJ2K)·손실
// 판정·subsampling은 디코딩 전에 머리로 판정한다. 라이브러리 실패는 할당 실패 표지로 OutOfMemory와
// 손상을 가른다.
namespace rocks::image::detail {
namespace {

constexpr uint32 ExrSlots = 4;
constexpr uint32 ExrAlphaSlot = 3;
constexpr int32 ExrNoChannel = -1;
constexpr int32 ExrNoSlot = -1;

struct ExrMemory {
    Span<const uint8> file;
};

int64_t ExrRead(exr_const_context_t, void* userData, void* buffer, uint64_t size, uint64_t offset,
                exr_stream_error_func_ptr_t) noexcept {
    const auto* memory = static_cast<const ExrMemory*>(userData);
    if (offset >= memory->file.Size()) {
        return 0;
    }

    const uint64 available = memory->file.Size() - offset;
    const uint64 count = size < available ? size : available;
    platform::MemoryCopy(buffer, memory->file.Data() + offset, static_cast<usize>(count));
    return static_cast<int64_t>(count);
}

int64_t ExrSize(exr_const_context_t, void* userData) noexcept {
    return static_cast<int64_t>(static_cast<const ExrMemory*>(userData)->file.Size());
}

// 라이브러리 메시지는 버린다(12.10절 공통 규칙 3 — 기본 처리기는 stderr에 쓴다).
void ExrMessage(exr_const_context_t, exr_result_t, const char*) noexcept {
}

class ExrContext {
public:
    ExrContext() noexcept = default;

    ~ExrContext() noexcept {
        if (context != nullptr) {
            exr_finish(&context);
        }
    }

    ExrContext(const ExrContext&) = delete;
    ExrContext& operator=(const ExrContext&) = delete;

    exr_context_t context = nullptr;
};

class ExrDecoding {
public:
    explicit ExrDecoding(exr_const_context_t context) noexcept : context_(context) {
    }

    ~ExrDecoding() noexcept {
        if (initialized) {
            exr_decoding_destroy(context_, &pipeline);
        }
    }

    ExrDecoding(const ExrDecoding&) = delete;
    ExrDecoding& operator=(const ExrDecoding&) = delete;

    // EXR_DECODE_PIPELINE_INITIALIZER({크기, 0})는 C++에서 -Wmissing-field-initializers라 값
    // 초기화한다 (pipe_size는 OpenEXR이 읽지 않는다).
    exr_decode_pipeline_t pipeline{};
    bool initialized = false;

private:
    exr_const_context_t context_;
};

Result<void> ExrLibraryFailure(const ExternalAllocationScope& scope,
                               ImageDecodeResult& out) noexcept {
    return scope.AllocationFailed() ? Resource(out, ErrorCode::OutOfMemory) : Invalid(out, 0);
}

// 채널 → 칸 대응이다(UE ExrImageWrapper.cpp 478–535). slots[칸] = 채널 index, ignored = 버린 채널
// 수.
struct ExrMapping {
    int32 slots[ExrSlots] = {ExrNoChannel, ExrNoChannel, ExrNoChannel, ExrNoChannel};
    uint32 slotCount = ExrSlots;
    uint32 ignored = 0;
};

Result<ExrMapping> ExrMapChannels(Allocator& allocator,
                                  const exr_attr_chlist_t& channels) noexcept {
    ExrMapping mapping;
    if (channels.num_channels == 1) {
        // 이름과 무관하게 칸 0 — 결과는 R16F·R32F다.
        mapping.slots[0] = 0;
        mapping.slotCount = 1;
        return Result<ExrMapping>::Success(mapping);
    }

    constexpr char Desired[ExrSlots] = {'R', 'G', 'B', 'A'};
    Array<int32> unused(allocator);
    for (int32 index = 0; index < channels.num_channels; ++index) {
        const exr_attr_string_t& name = channels.entries[index].name;
        char last = name.length > 0 ? name.str[name.length - 1] : '\0';
        if (last >= 'a' && last <= 'z') {
            last = static_cast<char>(last - 'a' + 'A');
        }

        bool placed = false;
        for (uint32 slot = 0; slot < ExrSlots; ++slot) {
            if (last == Desired[slot] && mapping.slots[slot] == ExrNoChannel) {
                mapping.slots[slot] = index;
                placed = true;
                break;
            }
        }

        if (!placed) {
            if (auto pushed = unused.TryPushBack(index); !pushed) {
                return Result<ExrMapping>::Failure(pushed.Error());
            }
        }
    }

    // 빈 칸은 남은 채널을 뒤에서부터 꺼내 채운다(UE FileChannelNamesUnused.Pop()).
    for (uint32 slot = 0; slot < ExrSlots; ++slot) {
        if (mapping.slots[slot] == ExrNoChannel && unused.Size() > 0) {
            mapping.slots[slot] = unused.TryPopBack().Value();
        }
    }

    mapping.ignored = static_cast<uint32>(unused.Size());
    return Result<ExrMapping>::Success(mapping);
}

bool ExrUsesType(const exr_attr_chlist_t& channels, const ExrMapping& mapping,
                 exr_pixel_type_t type) noexcept {
    for (uint32 slot = 0; slot < mapping.slotCount; ++slot) {
        const int32 index = mapping.slots[slot];
        if (index != ExrNoChannel && channels.entries[index].pixel_type == type) {
            return true;
        }
    }

    return false;
}

// 디코딩 전 압축 판정이다(12.10절): HTJ2K는 후속, B44·DWA는 사용 채널에 half가 있으면 손실, PXR24는
// 사용 채널에 float가 있으면 손실이다.
Result<void> ExrCheckCompression(exr_compression_t compression, const exr_attr_chlist_t& channels,
                                 const ExrMapping& mapping, ImageDecodeResult& out) noexcept {
    switch (compression) {
    case EXR_COMPRESSION_NONE:
    case EXR_COMPRESSION_RLE:
    case EXR_COMPRESSION_ZIPS:
    case EXR_COMPRESSION_ZIP:
    case EXR_COMPRESSION_PIZ:
        return Result<void>::Success();
    case EXR_COMPRESSION_PXR24:
        return ExrUsesType(channels, mapping, EXR_PIXEL_FLOAT)
                   ? Fail(out, AssetDiagnosticCode::LossySource, ErrorCode::Unsupported, 0)
                   : Result<void>::Success();
    case EXR_COMPRESSION_B44:
    case EXR_COMPRESSION_B44A:
    case EXR_COMPRESSION_DWAA:
    case EXR_COMPRESSION_DWAB:
        return ExrUsesType(channels, mapping, EXR_PIXEL_HALF)
                   ? Fail(out, AssetDiagnosticCode::LossySource, ErrorCode::Unsupported, 0)
                   : Result<void>::Success();
    default:
        // HTJ2K256·HTJ2K32(후속)와 모르는 값.
        return Unsupported(out, 0);
    }
}

// chunk 하나의 사용 채널을 결과 위치로 잇고 디코딩한다. originX·originY는 data window 기준 chunk
// 원점이다.
Result<void> ExrDecodeChunk(exr_context_t context, const exr_chunk_info_t& chunk, uint32 originX,
                            uint32 originY, const int32* slotOfChannel, bool half,
                            ExrDecoding& decoding, const ExternalAllocationScope& scope,
                            ImageDecodeResult& out) noexcept {
    exr_result_t result = decoding.initialized
                              ? exr_decoding_update(context, 0, &chunk, &decoding.pipeline)
                              : exr_decoding_initialize(context, 0, &chunk, &decoding.pipeline);
    if (result != EXR_ERR_SUCCESS) {
        return ExrLibraryFailure(scope, out);
    }

    const bool first = !decoding.initialized;
    decoding.initialized = true;
    const usize elementBytes = half ? 2 : 4;
    const usize texelBytes = TextureSourceTexelBytes(out.image.format);
    const usize lineBytes = static_cast<usize>(out.image.width) * texelBytes;
    uint8* origin = out.image.pixels.Data() + static_cast<usize>(originY) * lineBytes +
                    static_cast<usize>(originX) * texelBytes;
    for (int16_t index = 0; index < decoding.pipeline.channel_count; ++index) {
        exr_coding_channel_info_t& channel = decoding.pipeline.channels[index];
        const int32 slot = slotOfChannel[index];
        if (slot == ExrNoSlot) {
            channel.decode_to_ptr = nullptr;
            continue;
        }

        channel.user_data_type = static_cast<uint16_t>(half ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT);
        channel.user_bytes_per_element = static_cast<int16_t>(elementBytes);
        channel.user_pixel_stride = static_cast<int32_t>(texelBytes);
        channel.user_line_stride = static_cast<int32_t>(lineBytes);
        channel.decode_to_ptr = origin + static_cast<usize>(slot) * elementBytes;
    }

    if (first) {
        result = exr_decoding_choose_default_routines(context, 0, &decoding.pipeline);
        if (result != EXR_ERR_SUCCESS) {
            return ExrLibraryFailure(scope, out);
        }
    }

    result = exr_decoding_run(context, 0, &decoding.pipeline);
    return result == EXR_ERR_SUCCESS ? Result<void>::Success() : ExrLibraryFailure(scope, out);
}

} // namespace

Result<void> DecodeExr(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    ExternalAllocationScope scope(allocator);
    ExrMemory memory{file};
    exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
    initializer.error_handler_fn = ExrMessage;
    initializer.alloc_fn = ExternalAllocate;
    initializer.free_fn = ExternalFree;
    initializer.user_data = &memory;
    initializer.read_fn = ExrRead;
    initializer.size_fn = ExrSize;
    // 한 변 한도는 라이브러리에 주지 않는다(넘으면 머리 검증 실패가 InvalidArgument가 된다) —
    // 머리는 크기에 비례해 할당하지 않고, chunk 표는 첫 chunk에서 잡히므로 AllocateImage가 먼저
    // CapacityExceeded로 판정한다(12.9절과 같은 분류).
    initializer.max_image_width = 0;
    initializer.max_image_height = 0;
    ExrContext opened;
    if (exr_start_read(&opened.context, "memory", &initializer) != EXR_ERR_SUCCESS) {
        return ExrLibraryFailure(scope, out);
    }

    exr_context_t context = opened.context;
    int parts = 0;
    exr_storage_t storage = EXR_STORAGE_SCANLINE;
    exr_compression_t compression = EXR_COMPRESSION_NONE;
    exr_attr_box2i_t window{};
    const exr_attr_chlist_t* channels = nullptr;
    if (exr_get_count(context, &parts) != EXR_ERR_SUCCESS || parts < 1 ||
        exr_get_storage(context, 0, &storage) != EXR_ERR_SUCCESS ||
        exr_get_compression(context, 0, &compression) != EXR_ERR_SUCCESS ||
        exr_get_data_window(context, 0, &window) != EXR_ERR_SUCCESS ||
        exr_get_channels(context, 0, &channels) != EXR_ERR_SUCCESS || channels == nullptr) {
        return ExrLibraryFailure(scope, out);
    }

    if (storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED) {
        return Unsupported(out, 0);
    }

    if (storage != EXR_STORAGE_SCANLINE && storage != EXR_STORAGE_TILED) {
        return Invalid(out, 0);
    }

    if (channels->num_channels <= 0) {
        return Invalid(out, 0);
    }

    auto mapped = ExrMapChannels(allocator, *channels);
    if (!mapped) {
        return Resource(out, mapped.Error());
    }

    const ExrMapping mapping = mapped.Value();
    for (uint32 slot = 0; slot < mapping.slotCount; ++slot) {
        const int32 index = mapping.slots[slot];
        if (index != ExrNoChannel && (channels->entries[index].x_sampling != 1 ||
                                      channels->entries[index].y_sampling != 1)) {
            return Unsupported(out, 0);
        }
    }

    if (auto checked = ExrCheckCompression(compression, *channels, mapping, out); !checked) {
        return checked;
    }

    // 정밀도는 part 0의 모든 채널로 정한다(UE 276–294).
    bool allHalf = true;
    for (int32 index = 0; index < channels->num_channels; ++index) {
        allHalf = allHalf && channels->entries[index].pixel_type == EXR_PIXEL_HALF;
    }

    const int64 width = int64{window.max.x} - window.min.x + 1;
    const int64 height = int64{window.max.y} - window.min.y + 1;
    if (width <= 0 || height <= 0 || width > NumericLimits<uint32>::Max() ||
        height > NumericLimits<uint32>::Max()) {
        return Invalid(out, 0);
    }

    const bool single = mapping.slotCount == 1;
    const TextureSourceFormat format =
        single ? (allHalf ? TextureSourceFormat::R16F : TextureSourceFormat::R32F)
               : (allHalf ? TextureSourceFormat::RGBA16F : TextureSourceFormat::RGBA32F);
    if (auto allocated = AllocateImage(limits, format, static_cast<uint32>(width),
                                       static_cast<uint32>(height), out);
        !allocated) {
        return allocated;
    }

    // 채널 index → 칸(디코더 채널 순서는 채널 목록 순서와 같다).
    Array<int32> slotOfChannel(allocator);
    if (auto sized = slotOfChannel.TryResize(static_cast<usize>(channels->num_channels)); !sized) {
        return Resource(out, sized.Error());
    }

    for (int32& slot : slotOfChannel) {
        slot = ExrNoSlot;
    }

    for (uint32 slot = 0; slot < mapping.slotCount; ++slot) {
        if (mapping.slots[slot] != ExrNoChannel) {
            slotOfChannel[static_cast<usize>(mapping.slots[slot])] = static_cast<int32>(slot);
        }
    }

    ExrDecoding decoding(context);
    uint32 ignoredMips = 0;
    if (storage == EXR_STORAGE_TILED) {
        uint32_t tileWidth = 0;
        uint32_t tileHeight = 0;
        exr_tile_level_mode_t levelMode = EXR_TILE_ONE_LEVEL;
        exr_tile_round_mode_t roundMode = EXR_TILE_ROUND_DOWN;
        int32_t levelsX = 1;
        int32_t levelsY = 1;
        int32_t countX = 0;
        int32_t countY = 0;
        if (exr_get_tile_descriptor(context, 0, &tileWidth, &tileHeight, &levelMode, &roundMode) !=
                EXR_ERR_SUCCESS ||
            exr_get_tile_levels(context, 0, &levelsX, &levelsY) != EXR_ERR_SUCCESS ||
            exr_get_tile_counts(context, 0, 0, 0, &countX, &countY) != EXR_ERR_SUCCESS ||
            tileWidth == 0 || tileHeight == 0) {
            return ExrLibraryFailure(scope, out);
        }

        if (levelMode == EXR_TILE_MIPMAP_LEVELS && levelsX > 1) {
            ignoredMips = static_cast<uint32>(levelsX - 1);
        } else if (levelMode == EXR_TILE_RIPMAP_LEVELS && levelsX * levelsY > 1) {
            ignoredMips = static_cast<uint32>(levelsX * levelsY - 1);
        }

        for (int32_t tileY = 0; tileY < countY; ++tileY) {
            for (int32_t tileX = 0; tileX < countX; ++tileX) {
                exr_chunk_info_t chunk{};
                if (exr_read_tile_chunk_info(context, 0, tileX, tileY, 0, 0, &chunk) !=
                    EXR_ERR_SUCCESS) {
                    return ExrLibraryFailure(scope, out);
                }

                // tile chunk의 start는 tile 번호다 — 화소 원점은 번호 × tile 크기다.
                const uint64 originX = uint64{static_cast<uint32>(tileX)} * tileWidth;
                const uint64 originY = uint64{static_cast<uint32>(tileY)} * tileHeight;
                if (originX + static_cast<uint32>(chunk.width) > out.image.width ||
                    originY + static_cast<uint32>(chunk.height) > out.image.height) {
                    return Invalid(out, 0);
                }

                if (auto decoded = ExrDecodeChunk(
                        context, chunk, static_cast<uint32>(originX), static_cast<uint32>(originY),
                        slotOfChannel.Data(), allHalf, decoding, scope, out);
                    !decoded) {
                    return decoded;
                }
            }
        }
    } else {
        int32_t linesPerChunk = 0;
        if (exr_get_scanlines_per_chunk(context, 0, &linesPerChunk) != EXR_ERR_SUCCESS ||
            linesPerChunk <= 0) {
            return ExrLibraryFailure(scope, out);
        }

        for (int64 y = window.min.y; y <= window.max.y; y += linesPerChunk) {
            exr_chunk_info_t chunk{};
            if (exr_read_scanline_chunk_info(context, 0, static_cast<int>(y), &chunk) !=
                EXR_ERR_SUCCESS) {
                return ExrLibraryFailure(scope, out);
            }

            const int64 originY = int64{chunk.start_y} - window.min.y;
            if (originY < 0 || originY + chunk.height > height || chunk.width != width) {
                return Invalid(out, 0);
            }

            if (auto decoded = ExrDecodeChunk(context, chunk, 0, static_cast<uint32>(originY),
                                              slotOfChannel.Data(), allHalf, decoding, scope, out);
                !decoded) {
                return decoded;
            }
        }
    }

    // 빈 알파는 1, 빈 색은 0(할당 때 0)이다. 그다음 NaN·Inf를 정리한다(12.2절 — half도).
    const usize texelCount = static_cast<usize>(out.image.width) * out.image.height;
    const usize channelsPerTexel = single ? 1 : ExrSlots;
    const bool alphaMissing = !single && mapping.slots[ExrAlphaSlot] == ExrNoChannel;
    uint8* pixels = out.image.pixels.Data();
    for (usize texel = 0; texel < texelCount; ++texel) {
        for (usize c = 0; c < channelsPerTexel; ++c) {
            const usize at = (texel * channelsPerTexel + c) * (allHalf ? 2 : 4);
            if (allHalf) {
                const uint16 value = alphaMissing && c == ExrAlphaSlot
                                         ? Float16::FromFloat(1.0f).bits
                                         : LoadLe16(pixels + at);
                StoreLe16(pixels + at, SanitizeHalfBits(value));
            } else {
                const float value = alphaMissing && c == ExrAlphaSlot
                                        ? 1.0f
                                        : platform::BitCast<float>(LoadLe32(pixels + at));
                StoreSanitizedFloat(pixels + at, value);
            }
        }
    }

    if (mapping.ignored > 0) {
        if (auto warned = Warn(out, AssetDiagnosticCode::IgnoredSourceChannels, mapping.ignored);
            !warned) {
            return warned;
        }
    }

    if (ignoredMips > 0) {
        if (auto warned = Warn(out, AssetDiagnosticCode::IgnoredSourceMips, ignoredMips); !warned) {
            return warned;
        }
    }

    if (parts > 1) {
        return Warn(out, AssetDiagnosticCode::IgnoredSourceImages, static_cast<uint32>(parts - 1));
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
