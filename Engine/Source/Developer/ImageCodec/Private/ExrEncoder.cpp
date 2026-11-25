#include "ExternalAllocation.h"

#include <ImageCodec/ImageEncode.h>
#include <Platform/Memory.h>
#include <openexr.h>

// EXR 쓰기다(Docs/Design/AssetSystem 12.10.1절 — OpenEXRCore 쓰기 API를 메모리 파일에). 값 조합은
// OpenEXRCore가 받는 대로 쓴다. 할당은 디코더와 같은 스레드별 묶음(컨텍스트 alloc_fn·free_fn)이고
// 실패 표지로 OutOfMemory를 가른다.
namespace rocks::image {
namespace {

using detail::ExternalAllocate;
using detail::ExternalAllocationScope;
using detail::ExternalFree;

// 원소 byte 수다(Half 2, Uint·Float 4).
usize ExrWriteElementBytes(ExrPixelType type) noexcept {
    return type == ExrPixelType::Half ? 2 : 4;
}

struct ExrWriteMemory {
    explicit ExrWriteMemory(Array<uint8>& bytes) noexcept : data(bytes) {
    }

    Array<uint8>& data;
    bool failed = false;
};

int64_t ExrWriteStream(exr_const_context_t, void* userData, const void* buffer, uint64_t size,
                       uint64_t offset, exr_stream_error_func_ptr_t) noexcept {
    auto* file = static_cast<ExrWriteMemory*>(userData);
    if (offset + size > file->data.Size() &&
        !file->data.TryResize(static_cast<usize>(offset + size))) {
        file->failed = true;
        return -1;
    }

    platform::MemoryCopy(file->data.Data() + offset, buffer, static_cast<usize>(size));
    return static_cast<int64_t>(size);
}

void ExrWriteMessage(exr_const_context_t, exr_result_t, const char*) noexcept {
}

const ExrChannelWrite* ExrWriteFindChannel(const ExrWriteImage& image, const char* name) noexcept {
    for (const ExrChannelWrite& channel : image.channels) {
        if (channel.name == StringView(name)) {
            return &channel;
        }
    }

    return nullptr;
}

// chunk 하나를 부호화한다. level0이 아니면(tile level > 0) zeros를 쓴다.
bool ExrWriteChunk(exr_context_t context, int part, const exr_chunk_info_t& chunk,
                   const ExrWriteImage& image, int32 originX, int32 originY,
                   Span<const uint8> zeros, bool level0, exr_encode_pipeline_t& pipeline,
                   bool& initialized) noexcept {
    const exr_result_t result = initialized
                                    ? exr_encoding_update(context, part, &chunk, &pipeline)
                                    : exr_encoding_initialize(context, part, &chunk, &pipeline);
    if (result != EXR_ERR_SUCCESS) {
        return false;
    }

    const bool first = !initialized;
    initialized = true;
    for (int16_t index = 0; index < pipeline.channel_count; ++index) {
        exr_coding_channel_info_t& channel = pipeline.channels[index];
        const ExrChannelWrite* source = ExrWriteFindChannel(image, channel.channel_name);
        if (source == nullptr) {
            return false;
        }

        const usize element = ExrWriteElementBytes(source->type);
        const int32 rowElements = image.width / source->sampling;
        channel.user_data_type = static_cast<uint16_t>(source->type);
        channel.user_bytes_per_element = static_cast<int16_t>(element);
        channel.user_pixel_stride = static_cast<int32_t>(element);
        channel.user_line_stride = static_cast<int32_t>(rowElements * static_cast<int32>(element));
        const usize at = (static_cast<usize>(originY / source->sampling) * rowElements +
                          static_cast<usize>(originX / source->sampling)) *
                         element;
        channel.encode_from_ptr = level0 ? source->data.Data() + at : zeros.Data();
        if (!level0) {
            channel.user_line_stride =
                static_cast<int32_t>(image.width * static_cast<int32>(element));
        }
    }

    if (first &&
        exr_encoding_choose_default_routines(context, part, &pipeline) != EXR_ERR_SUCCESS) {
        return false;
    }

    return exr_encoding_run(context, part, &pipeline) == EXR_ERR_SUCCESS;
}

// 한 part의 chunk를 모두 쓴다.
bool ExrWritePart(exr_context_t context, int part, const ExrWriteImage& image,
                  Span<const uint8> zeros) noexcept {
    exr_encode_pipeline_t pipeline{};
    bool initialized = false;
    bool ok = true;
    if (image.storage == ExrStorage::Tiled) {
        int32_t levelsX = 1;
        int32_t levelsY = 1;
        ok = exr_get_tile_levels(context, part, &levelsX, &levelsY) == EXR_ERR_SUCCESS;
        for (int32_t level = 0; ok && level < levelsX; ++level) {
            int32_t countX = 0;
            int32_t countY = 0;
            ok = exr_get_tile_counts(context, part, level, level, &countX, &countY) ==
                 EXR_ERR_SUCCESS;
            // RandomY는 tile을 거꾸로 쓴다.
            for (int32_t step = 0; ok && step < countX * countY; ++step) {
                const int32_t order =
                    image.lineOrder == ExrLineOrder::RandomY ? countX * countY - 1 - step : step;
                const int32_t tileX = order % countX;
                const int32_t tileY = order / countX;
                exr_chunk_info_t chunk{};
                ok = exr_write_tile_chunk_info(context, part, tileX, tileY, level, level, &chunk) ==
                         EXR_ERR_SUCCESS &&
                     ExrWriteChunk(context, part, chunk, image,
                                   tileX * static_cast<int32>(image.tileWidth),
                                   tileY * static_cast<int32>(image.tileHeight), zeros, level == 0,
                                   pipeline, initialized);
            }

            if (image.levels != ExrLevelMode::MipmapLevels) {
                break;
            }
        }
    } else {
        int32_t lines = 0;
        ok = exr_get_scanlines_per_chunk(context, part, &lines) == EXR_ERR_SUCCESS && lines > 0;
        const int32 chunks = ok ? (image.height + lines - 1) / lines : 0;
        for (int32 step = 0; ok && step < chunks; ++step) {
            const int32 chunkIndex =
                image.lineOrder == ExrLineOrder::DecreasingY ? chunks - 1 - step : step;
            exr_chunk_info_t chunk{};
            ok = exr_write_scanline_chunk_info(context, part, image.minY + chunkIndex * lines,
                                               &chunk) == EXR_ERR_SUCCESS &&
                 ExrWriteChunk(context, part, chunk, image, 0, chunkIndex * lines, zeros, true,
                               pipeline, initialized);
        }
    }

    if (initialized) {
        exr_encoding_destroy(context, &pipeline);
    }

    return ok;
}

} // namespace

Result<void> EncodeExr(Allocator& allocator, const ExrWriteImage& image,
                       Array<uint8>& out) noexcept {
    out.Clear();
    if (image.width <= 0 || image.height <= 0 || image.channels.IsEmpty() || image.parts < 1 ||
        image.parts > ExrWriteMaxParts ||
        (image.storage == ExrStorage::Tiled && (image.tileWidth == 0 || image.tileHeight == 0))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 이름은 NUL로 끝나는 C 문자열로 넘기므로 길이를 묶는다(채널 이름은 EXR 32 byte 이하가 보통).
    constexpr usize MaxChannelNameBytes = 255;
    for (const ExrChannelWrite& channel : image.channels) {
        const usize rows =
            static_cast<usize>(image.height / (channel.sampling > 0 ? channel.sampling : 1));
        const usize columns =
            static_cast<usize>(image.width / (channel.sampling > 0 ? channel.sampling : 1));
        if (channel.sampling < 1 || channel.name.IsEmpty() ||
            channel.name.ByteCount() > MaxChannelNameBytes ||
            channel.data.Size() < rows * columns * ExrWriteElementBytes(channel.type)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    ExternalAllocationScope scope(allocator);
    const auto failed = [&](exr_context_t& context) noexcept {
        if (context != nullptr) {
            exr_finish(&context);
        }

        out.Clear();
        return Result<void>::Failure(scope.AllocationFailed() ? ErrorCode::OutOfMemory
                                                              : ErrorCode::InvalidArgument);
    };
    ExrWriteMemory file(out);
    exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
    initializer.error_handler_fn = ExrWriteMessage;
    initializer.alloc_fn = ExternalAllocate;
    initializer.free_fn = ExternalFree;
    initializer.user_data = &file;
    initializer.write_fn = ExrWriteStream;
    exr_context_t context = nullptr;
    if (exr_start_write(&context, "memory", EXR_WRITE_FILE_DIRECTLY, &initializer) !=
        EXR_ERR_SUCCESS) {
        return failed(context);
    }

    for (int32 part = 0; part < image.parts; ++part) {
        const char names[ExrWriteMaxParts][2] = {"a", "b", "c"};
        int index = 0;
        if (exr_add_part(context, names[part], static_cast<exr_storage_t>(image.storage), &index) !=
            EXR_ERR_SUCCESS) {
            return failed(context);
        }

        const exr_attr_box2i_t window{
            {image.minX, image.minY},
            {image.minX + image.width - 1, image.minY + image.height - 1}};
        const exr_attr_v2f_t center{0.0f, 0.0f};
        if (exr_initialize_required_attr(context, index, &window, &window, 1.0f, &center, 1.0f,
                                         static_cast<exr_lineorder_t>(image.lineOrder),
                                         static_cast<exr_compression_t>(image.compression)) !=
            EXR_ERR_SUCCESS) {
            return failed(context);
        }

        for (const ExrChannelWrite& channel : image.channels) {
            char name[MaxChannelNameBytes + 1];
            platform::MemoryCopy(name, channel.name.Data(), channel.name.ByteCount());
            name[channel.name.ByteCount()] = '\0';
            if (exr_add_channel(context, index, name, static_cast<exr_pixel_type_t>(channel.type),
                                EXR_PERCEPTUALLY_LOGARITHMIC, channel.sampling,
                                channel.sampling) != EXR_ERR_SUCCESS) {
                return failed(context);
            }
        }

        if (image.storage == ExrStorage::Tiled &&
            exr_set_tile_descriptor(context, index, image.tileWidth, image.tileHeight,
                                    static_cast<exr_tile_level_mode_t>(image.levels),
                                    EXR_TILE_ROUND_DOWN) != EXR_ERR_SUCCESS) {
            return failed(context);
        }
    }

    if (exr_write_header(context) != EXR_ERR_SUCCESS) {
        return failed(context);
    }

    // deep은 머리만 쓴다(디코더 거부 시험용).
    if (image.storage == ExrStorage::Scanline || image.storage == ExrStorage::Tiled) {
        Array<uint8> zeros(allocator);
        if (!zeros.TryResize(static_cast<usize>(image.width) * static_cast<usize>(image.height) *
                             4)) {
            return failed(context);
        }

        platform::MemoryZero(zeros.Data(), zeros.Size());
        for (int32 part = 0; part < image.parts; ++part) {
            if (!ExrWritePart(context, part, image, zeros.View())) {
                return failed(context);
            }
        }
    }

    if (exr_finish(&context) != EXR_ERR_SUCCESS || file.failed || scope.AllocationFailed()) {
        out.Clear();
        return Result<void>::Failure(scope.AllocationFailed() ? ErrorCode::OutOfMemory
                                                              : ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::image
