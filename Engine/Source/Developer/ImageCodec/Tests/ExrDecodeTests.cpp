#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <openexr.h>

// OpenEXR 디코더 시험이다(Docs/Design/AssetSystem 12.10절 A5f — OpenEXRCore, UE ExrImageWrapper와
// 같은 채널 규칙). 파일은 OpenEXRCore 쓰기 API로 메모리에 만든다(압축별·scanline·tile·mip
// level·line order·data window 원점·여러 part). 채널 대응(마지막 글자·뒤에서부터 채움·단일 채널),
// 정밀도(part 0 전체), 손실 판정(사용 채널 기준), HTJ2K·deep·subsampling 거부, NaN·Inf 정리, 손상이
// OutOfMemory가 아닌지, 절단, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::image;
namespace exr_decode_tests_file {

namespace {

constexpr usize TestBlockHeader = 16;

void* TestExrAllocate(size_t bytes) noexcept {
    void* block = platform::AlignedAllocate(bytes + TestBlockHeader, TestBlockHeader);
    return block == nullptr ? nullptr : static_cast<uint8*>(block) + TestBlockHeader;
}

void TestExrFree(void* address) noexcept {
    if (address != nullptr) {
        platform::AlignedFree(static_cast<uint8*>(address) - TestBlockHeader);
    }
}

struct MemoryFile {
    explicit MemoryFile(Allocator& allocator) noexcept : data(allocator) {
    }

    Array<uint8> data;
};

int64_t MemoryWrite(exr_const_context_t, void* userData, const void* buffer, uint64_t size,
                    uint64_t offset, exr_stream_error_func_ptr_t) noexcept {
    auto* file = static_cast<MemoryFile*>(userData);
    if (offset + size > file->data.Size() &&
        !file->data.TryResize(static_cast<usize>(offset + size))) {
        return -1;
    }

    platform::MemoryCopy(file->data.Data() + offset, buffer, static_cast<usize>(size));
    return static_cast<int64_t>(size);
}

void IgnoreMessage(exr_const_context_t, exr_result_t, const char*) noexcept {
}

// 채널 하나다. data는 data window의 행 우선 원소(half·uint·float — 2·4·4 byte)다.
struct ExrChannelSpec {
    const char* name;
    exr_pixel_type_t type;
    Span<const uint8> data;
    int32 sampling = 1;
};

struct ExrSpec {
    int32 minX = 0;
    int32 minY = 0;
    int32 width = 1;
    int32 height = 1;
    exr_compression_t compression = EXR_COMPRESSION_NONE;
    bool tiled = false;
    uint32 tileWidth = 0;
    uint32 tileHeight = 0;
    exr_tile_level_mode_t levels = EXR_TILE_ONE_LEVEL;
    exr_lineorder_t lineOrder = EXR_LINEORDER_INCREASING_Y;
    exr_storage_t storage = EXR_STORAGE_SCANLINE;
    Span<const ExrChannelSpec> channels;
    int parts = 1;
};

usize ElementBytes(exr_pixel_type_t type) noexcept {
    return type == EXR_PIXEL_HALF ? 2 : 4;
}

const ExrChannelSpec* FindChannel(const ExrSpec& spec, const char* name) noexcept {
    for (const ExrChannelSpec& channel : spec.channels) {
        if (platform::CStringEquals(channel.name, name)) {
            return &channel;
        }
    }

    return nullptr;
}

// chunk 하나를 쓴다. level > 0의 tile은 0으로 채운 zeros를 쓴다.
bool EncodeChunk(exr_context_t context, int part, const exr_chunk_info_t& chunk,
                 const ExrSpec& spec, int32 originX, int32 originY, Span<const uint8> zeros,
                 bool level0, exr_encode_pipeline_t& pipeline, bool& initialized) noexcept {
    exr_result_t result = initialized ? exr_encoding_update(context, part, &chunk, &pipeline)
                                      : exr_encoding_initialize(context, part, &chunk, &pipeline);
    if (result != EXR_ERR_SUCCESS) {
        return false;
    }

    const bool first = !initialized;
    initialized = true;
    for (int16_t index = 0; index < pipeline.channel_count; ++index) {
        exr_coding_channel_info_t& channel = pipeline.channels[index];
        const ExrChannelSpec* source = FindChannel(spec, channel.channel_name);
        const usize element = ElementBytes(source->type);
        const int32 rowElements = spec.width / source->sampling;
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
                static_cast<int32_t>(spec.width * static_cast<int32>(element));
        }
    }

    if (first &&
        exr_encoding_choose_default_routines(context, part, &pipeline) != EXR_ERR_SUCCESS) {
        return false;
    }

    return exr_encoding_run(context, part, &pipeline) == EXR_ERR_SUCCESS;
}

Array<uint8> WriteExr(Allocator& allocator, const ExrSpec& spec) noexcept {
    MemoryFile file(allocator);
    exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
    initializer.error_handler_fn = IgnoreMessage;
    initializer.alloc_fn = TestExrAllocate;
    initializer.free_fn = TestExrFree;
    initializer.user_data = &file;
    initializer.write_fn = MemoryWrite;
    exr_context_t context = nullptr;
    CHECK(exr_start_write(&context, "test", EXR_WRITE_FILE_DIRECTLY, &initializer) ==
          EXR_ERR_SUCCESS);
    for (int part = 0; part < spec.parts; ++part) {
        const char names[][2] = {"a", "b", "c"};
        int index = 0;
        CHECK(exr_add_part(context, names[part], spec.storage, &index) == EXR_ERR_SUCCESS);
        const exr_attr_box2i_t window{{spec.minX, spec.minY},
                                      {spec.minX + spec.width - 1, spec.minY + spec.height - 1}};
        const exr_attr_v2f_t center{0.0f, 0.0f};
        CHECK(exr_initialize_required_attr(context, index, &window, &window, 1.0f, &center, 1.0f,
                                           spec.lineOrder, spec.compression) == EXR_ERR_SUCCESS);
        for (const ExrChannelSpec& channel : spec.channels) {
            CHECK(exr_add_channel(context, index, channel.name, channel.type,
                                  EXR_PERCEPTUALLY_LOGARITHMIC, channel.sampling,
                                  channel.sampling) == EXR_ERR_SUCCESS);
        }

        if (spec.tiled) {
            CHECK(exr_set_tile_descriptor(context, index, spec.tileWidth, spec.tileHeight,
                                          spec.levels, EXR_TILE_ROUND_DOWN) == EXR_ERR_SUCCESS);
        }
    }

    CHECK(exr_write_header(context) == EXR_ERR_SUCCESS);
    if (spec.storage != EXR_STORAGE_SCANLINE && spec.storage != EXR_STORAGE_TILED) {
        exr_finish(&context);
        return Move(file.data);
    }

    Array<uint8> zeros(allocator);
    CHECK(zeros.TryResize(static_cast<usize>(spec.width) * spec.height * 4));
    platform::MemoryZero(zeros.Data(), zeros.Size());
    for (int part = 0; part < spec.parts; ++part) {
        exr_encode_pipeline_t pipeline = EXR_ENCODE_PIPELINE_INITIALIZER;
        bool initialized = false;
        if (spec.tiled) {
            int32_t levelsX = 1;
            int32_t levelsY = 1;
            CHECK(exr_get_tile_levels(context, part, &levelsX, &levelsY) == EXR_ERR_SUCCESS);
            for (int32_t level = 0; level < levelsX; ++level) {
                int32_t countX = 0;
                int32_t countY = 0;
                CHECK(exr_get_tile_counts(context, part, level, level, &countX, &countY) ==
                      EXR_ERR_SUCCESS);
                // RANDOM_Y는 tile을 거꾸로 쓴다.
                for (int32_t step = 0; step < countX * countY; ++step) {
                    const int32_t order = spec.lineOrder == EXR_LINEORDER_RANDOM_Y
                                              ? countX * countY - 1 - step
                                              : step;
                    const int32_t tileX = order % countX;
                    const int32_t tileY = order / countX;
                    exr_chunk_info_t chunk{};
                    CHECK(exr_write_tile_chunk_info(context, part, tileX, tileY, level, level,
                                                    &chunk) == EXR_ERR_SUCCESS);
                    CHECK(EncodeChunk(context, part, chunk, spec,
                                      tileX * static_cast<int32>(spec.tileWidth),
                                      tileY * static_cast<int32>(spec.tileHeight), zeros.View(),
                                      level == 0, pipeline, initialized));
                }

                if (spec.levels != EXR_TILE_MIPMAP_LEVELS) {
                    break;
                }
            }
        } else {
            int32_t lines = 0;
            CHECK(exr_get_scanlines_per_chunk(context, part, &lines) == EXR_ERR_SUCCESS);
            const int32 chunks = (spec.height + lines - 1) / lines;
            for (int32 step = 0; step < chunks; ++step) {
                const int32 chunkIndex =
                    spec.lineOrder == EXR_LINEORDER_DECREASING_Y ? chunks - 1 - step : step;
                exr_chunk_info_t chunk{};
                CHECK(exr_write_scanline_chunk_info(context, part, spec.minY + chunkIndex * lines,
                                                    &chunk) == EXR_ERR_SUCCESS);
                CHECK(EncodeChunk(context, part, chunk, spec, 0, chunkIndex * lines, zeros.View(),
                                  true, pipeline, initialized));
            }
        }

        if (initialized) {
            exr_encoding_destroy(context, &pipeline);
        }
    }

    CHECK(exr_finish(&context) == EXR_ERR_SUCCESS);
    return Move(file.data);
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Unknown, {}, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool HasWarning(const Decoded& decoded, AssetDiagnosticCode code, uint32 detail) noexcept {
    for (const ImageDecodeWarning& warning : decoded.result.warnings) {
        if (warning.code == code && warning.detail == detail) {
            return true;
        }
    }

    return false;
}

uint16 Half(const Decoded& decoded, usize element) noexcept {
    const uint8* at = decoded.result.image.pixels.Data() + element * 2;
    return static_cast<uint16>(at[0] | (at[1] << 8));
}

float Single(const Decoded& decoded, usize element) noexcept {
    const uint8* at = decoded.result.image.pixels.Data() + element * 4;
    return platform::BitCast<float>(static_cast<uint32>(at[0]) | (static_cast<uint32>(at[1]) << 8) |
                                    (static_cast<uint32>(at[2]) << 16) |
                                    (static_cast<uint32>(at[3]) << 24));
}

// half 값 count개를 담는다.
struct Halves {
    explicit Halves(float base, usize count) noexcept : size(count) {
        for (usize index = 0; index < count; ++index) {
            values[index] = Float16::FromFloat(base + static_cast<float>(index)).bits;
        }
    }

    Span<const uint8> Bytes() const noexcept {
        return Span<const uint8>(reinterpret_cast<const uint8*>(values), size * 2);
    }

    uint16 values[64] = {};
    usize size;
};

struct Floats {
    explicit Floats(float base, usize count) noexcept : size(count) {
        for (usize index = 0; index < count; ++index) {
            values[index] = base + static_cast<float>(index) * 0.5f;
        }
    }

    Span<const uint8> Bytes() const noexcept {
        return Span<const uint8>(reinterpret_cast<const uint8*>(values), size * 4);
    }

    float values[64] = {};
    usize size;
};

// 머리의 compression 속성 값을 바꾼다(이름·형식 "compression\0compression\0" + 크기 1 + 값).
void PatchCompression(Array<uint8>& file, uint8 value) noexcept {
    const char key[] = "compression\0compression";
    constexpr usize KeyBytes = sizeof(key);
    for (usize at = 0; at + KeyBytes + 4 < file.Size(); ++at) {
        if (platform::MemoryEquals(file.Data() + at, key, KeyBytes)) {
            file[at + KeyBytes + 4] = value;
            return;
        }
    }

    CHECK(false);
}

} // namespace

// 채널 대응(UE ExrImageWrapper 478–535)과 정밀도(part 0의 모든 채널).
TEST(ExrDecode_ChannelMapping) {
    SystemAllocator allocator;
    const Halves r(1.0f, 4);
    const Halves g(10.0f, 4);
    const Halves b(20.0f, 4);
    const Halves a(30.0f, 4);
    ExrSpec spec;
    spec.width = 2;
    spec.height = 2;
    // 채널 하나면 이름과 무관하게 단일 채널(R16F).
    const ExrChannelSpec single[] = {{"Z", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = single;
    Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::R16F &&
          Half(decoded, 3) == r.values[3]);

    // B·G·R: 알파가 없으면 1.
    const ExrChannelSpec rgb[] = {{"B", EXR_PIXEL_HALF, b.Bytes()},
                                  {"G", EXR_PIXEL_HALF, g.Bytes()},
                                  {"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = rgb;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA16F);
    CHECK(Half(decoded, 4) == r.values[1] && Half(decoded, 5) == g.values[1] &&
          Half(decoded, 6) == b.values[1] && Half(decoded, 7) == Float16::FromFloat(1.0f).bits);

    // X·Y·Z는 뒤에서부터 채운다 → (Z, Y, X, 1).
    const ExrChannelSpec xyz[] = {{"X", EXR_PIXEL_HALF, r.Bytes()},
                                  {"Y", EXR_PIXEL_HALF, g.Bytes()},
                                  {"Z", EXR_PIXEL_HALF, b.Bytes()}};
    spec.channels = xyz;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == b.values[0] && Half(decoded, 1) == g.values[0] &&
          Half(decoded, 2) == r.values[0]);

    // Y + A → (Y, 0, 0, A).
    const ExrChannelSpec ya[] = {{"Y", EXR_PIXEL_HALF, g.Bytes()},
                                 {"A", EXR_PIXEL_HALF, a.Bytes()}};
    spec.channels = ya;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == g.values[0] && Half(decoded, 1) == 0 &&
          Half(decoded, 2) == 0 && Half(decoded, 3) == a.values[0]);

    // 레이어 이름도 마지막 글자로 대응한다.
    const ExrChannelSpec layer[] = {{"diffuse.B", EXR_PIXEL_HALF, b.Bytes()},
                                    {"diffuse.G", EXR_PIXEL_HALF, g.Bytes()},
                                    {"diffuse.R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = layer;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == r.values[0] && Half(decoded, 2) == b.values[0]);

    // 다섯 채널: 남는 채널은 경고와 함께 버리고, 그 채널이 FLOAT면 정밀도는 32F다(part 0 전체).
    const Floats q(5.0f, 4);
    const ExrChannelSpec five[] = {{"A", EXR_PIXEL_HALF, a.Bytes()},
                                   {"B", EXR_PIXEL_HALF, b.Bytes()},
                                   {"G", EXR_PIXEL_HALF, g.Bytes()},
                                   {"Q", EXR_PIXEL_FLOAT, q.Bytes()},
                                   {"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = five;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F &&
          HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceChannels, 1));
    CHECK(Single(decoded, 0) == Float16::FromBits(r.values[0]).ToFloat() &&
          Single(decoded, 3) == Float16::FromBits(a.values[0]).ToFloat());

    // UINT 채널은 float로 바꾼다.
    const uint32 uints[4] = {7, 8, 9, 10};
    const ExrChannelSpec uintChannels[] = {
        {"G", EXR_PIXEL_UINT,
         Span<const uint8>(reinterpret_cast<const uint8*>(uints), sizeof(uints))},
        {"R", EXR_PIXEL_UINT,
         Span<const uint8>(reinterpret_cast<const uint8*>(uints), sizeof(uints))}};
    spec.channels = uintChannels;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F &&
          Single(decoded, 4) == 8.0f);

    // subsampling 채널을 쓰면 받지 않는다.
    const Halves small(1.0f, 1);
    const ExrChannelSpec sub[] = {{"G", EXR_PIXEL_HALF, g.Bytes()},
                                  {"R", EXR_PIXEL_HALF, small.Bytes(), 2}};
    spec.channels = sub;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 압축(무손실은 같은 값, 손실 판정은 사용 채널 기준), HTJ2K·deep 거부.
TEST(ExrDecode_Compression) {
    SystemAllocator allocator;
    Halves r(0.0f, 64);
    for (usize index = 0; index < 64; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index % 13) * 0.25f).bits;
    }

    ExrSpec spec;
    spec.width = 8;
    spec.height = 8;
    const ExrChannelSpec halfChannels[] = {{"G", EXR_PIXEL_HALF, r.Bytes()},
                                           {"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = halfChannels;
    const Decoded reference = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(reference.ok);
    const exr_compression_t lossless[] = {EXR_COMPRESSION_RLE, EXR_COMPRESSION_ZIPS,
                                          EXR_COMPRESSION_ZIP, EXR_COMPRESSION_PIZ,
                                          EXR_COMPRESSION_PXR24};
    for (exr_compression_t compression : lossless) {
        spec.compression = compression;
        const Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
        CHECK(decoded.ok && platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                                   reference.result.image.pixels.Data(),
                                                   reference.result.image.pixels.Size()));
    }

    // B44·DWA는 half 사용 채널이 있으면 손실이다.
    const exr_compression_t halfLossy[] = {EXR_COMPRESSION_B44, EXR_COMPRESSION_B44A,
                                           EXR_COMPRESSION_DWAA, EXR_COMPRESSION_DWAB};
    for (exr_compression_t compression : halfLossy) {
        spec.compression = compression;
        CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                    AssetDiagnosticCode::LossySource));
    }

    // FLOAT만이면 B44는 받고(그대로 저장한다) PXR24는 손실이다.
    const Floats f(0.5f, 64);
    const ExrChannelSpec floatChannels[] = {{"R", EXR_PIXEL_FLOAT, f.Bytes()}};
    spec.channels = floatChannels;
    spec.compression = EXR_COMPRESSION_B44;
    Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Single(decoded, 9) == f.values[9]);
    spec.compression = EXR_COMPRESSION_PXR24;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::LossySource));
    // 사용하지 않는 채널의 형식은 손실 판정에 들지 않는다: half R·G·B·A + 버리는 FLOAT Q는 PXR24로
    // 받는다.
    const ExrChannelSpec withUnused[] = {{"A", EXR_PIXEL_HALF, r.Bytes()},
                                         {"B", EXR_PIXEL_HALF, r.Bytes()},
                                         {"G", EXR_PIXEL_HALF, r.Bytes()},
                                         {"Q", EXR_PIXEL_FLOAT, f.Bytes()},
                                         {"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = withUnused;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceChannels, 1));

    // HTJ2K: 디코딩 전에 UnsupportedSourceFormat(InvalidSource가 아니다).
    spec.channels = halfChannels;
    spec.compression = EXR_COMPRESSION_NONE;
    Array<uint8> htj2k = WriteExr(allocator, spec);
    PatchCompression(htj2k, static_cast<uint8>(EXR_COMPRESSION_HTJ2K256));
    CHECK(Fails(Decode(allocator, htj2k.View()), AssetDiagnosticCode::UnsupportedSourceFormat));
    PatchCompression(htj2k, static_cast<uint8>(EXR_COMPRESSION_HTJ2K32));
    CHECK(Fails(Decode(allocator, htj2k.View()), AssetDiagnosticCode::UnsupportedSourceFormat));

    // deep scanline은 받지 않는다.
    spec.storage = EXR_STORAGE_DEEP_SCANLINE;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 배치: tile(부분 tile·RANDOM_Y·mip level), DECREASING_Y, data window 원점, 여러 part, NaN·Inf.
TEST(ExrDecode_Layout) {
    SystemAllocator allocator;
    Halves r(0.0f, 0);
    r.size = 60;
    for (usize index = 0; index < 60; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index)).bits;
    }

    ExrSpec spec;
    spec.width = 10;
    spec.height = 6;
    const ExrChannelSpec channels[] = {{"G", EXR_PIXEL_HALF, r.Bytes()},
                                       {"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = channels;
    const Decoded reference = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(reference.ok && Half(reference, (9 + 5 * 10) * 4) == r.values[59]);
    const auto same = [&reference](const Decoded& decoded) noexcept {
        return decoded.ok &&
               decoded.result.image.pixels.Size() == reference.result.image.pixels.Size() &&
               platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                      reference.result.image.pixels.Data(),
                                      reference.result.image.pixels.Size());
    };

    spec.lineOrder = EXR_LINEORDER_DECREASING_Y;
    spec.compression = EXR_COMPRESSION_ZIP;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = EXR_LINEORDER_INCREASING_Y;
    spec.minX = -3;
    spec.minY = 5;
    const Decoded shifted = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(shifted) && shifted.result.image.width == 10 && shifted.result.image.height == 6);
    spec.minX = 0;
    spec.minY = 0;

    // tile 4×4(가장자리 부분 tile), RANDOM_Y, mip level(버린 level 수 경고).
    spec.tiled = true;
    spec.storage = EXR_STORAGE_TILED;
    spec.tileWidth = 4;
    spec.tileHeight = 4;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = EXR_LINEORDER_RANDOM_Y;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = EXR_LINEORDER_INCREASING_Y;
    spec.levels = EXR_TILE_MIPMAP_LEVELS;
    const Decoded mips = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(mips) && HasWarning(mips, AssetDiagnosticCode::IgnoredSourceMips, 3));
    spec.levels = EXR_TILE_ONE_LEVEL;
    spec.tiled = false;
    spec.storage = EXR_STORAGE_SCANLINE;

    // 여러 part: part 0만, IgnoredSourceImages.
    spec.parts = 2;
    const Decoded parts = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(parts) && HasWarning(parts, AssetDiagnosticCode::IgnoredSourceImages, 1));
    spec.parts = 1;

    // half NaN → 0, +Inf → 65504.
    Halves special(0.0f, 0);
    special.size = 2;
    special.values[0] = 0x7E00;
    special.values[1] = 0x7C00;
    ExrSpec tiny;
    tiny.width = 2;
    const ExrChannelSpec specialChannel[] = {{"R", EXR_PIXEL_HALF, special.Bytes()}};
    tiny.channels = specialChannel;
    const Decoded cleaned = Decode(allocator, WriteExr(allocator, tiny).View());
    CHECK(cleaned.ok && Half(cleaned, 0) == 0 && Half(cleaned, 1) == 0x7BFF);
}

// 손상(chunk 내용이 깨지면 OutOfMemory가 아니라 InvalidSource), 절단, 할당 실패.
TEST(ExrDecode_DamageAndAllocation) {
    SystemAllocator allocator;
    Halves r(0.0f, 0);
    r.size = 64;
    for (usize index = 0; index < 64; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index % 7)).bits;
    }

    ExrSpec spec;
    spec.width = 8;
    spec.height = 8;
    const ExrChannelSpec channels[] = {{"R", EXR_PIXEL_HALF, r.Bytes()}};
    spec.channels = channels;
    const exr_compression_t compressions[] = {EXR_COMPRESSION_ZIP, EXR_COMPRESSION_PIZ};
    for (exr_compression_t compression : compressions) {
        spec.compression = compression;
        Array<uint8> file = WriteExr(allocator, spec);
        CHECK(Decode(allocator, file.View()).ok);
        // 마지막 chunk 내용의 뒤쪽 byte를 뒤집는다(deflate 넘침·Huffman 손상 경로).
        for (usize back = 1; back <= 8 && back < file.Size(); ++back) {
            file[file.Size() - back] = static_cast<uint8>(file[file.Size() - back] ^ 0xA5);
        }

        const Decoded damaged = Decode(allocator, file.View());
        CHECK(damaged.ok || damaged.result.failure.code == AssetDiagnosticCode::InvalidSource);
        CHECK(!(!damaged.ok && damaged.result.failure.error == ErrorCode::OutOfMemory));
    }

    spec.compression = EXR_COMPRESSION_ZIP;
    const Array<uint8> file = WriteExr(allocator, spec);
    for (usize length = 0; length < file.Size(); ++length) {
        CHECK(!Decode(allocator, file.View().Slice(0, length)).ok);
    }

    const Decoded reference = Decode(allocator, file.View());
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ImageDecodeResult result(failing);
        auto decoded = DecodeImage(failing, file.View(), ImageFileFormat::Exr, {}, result);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(decoded);
            break;
        }

        if (decoded) {
            CHECK(platform::MemoryEquals(result.image.pixels.Data(),
                                         reference.result.image.pixels.Data(),
                                         reference.result.image.pixels.Size()));
            continue;
        }

        CHECK(decoded.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace exr_decode_tests_file
