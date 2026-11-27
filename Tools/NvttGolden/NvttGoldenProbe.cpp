// NVTT 2.0.8 block compression golden probe (Docs/Design/AssetSystem section 12.17.4 item 3).
//
// Builds the NVTT 2.0.8 sources from an Unreal Engine 5.8 clone
// (Engine/Source/ThirdParty/nvTextureTools) in one translation unit (plus NvttGoldenOptimal.cpp)
// with the same settings as the UE non-AVX2 DLL (__SSE2__, no AVX2, no FMA, SQUISH_USE_DIV) and
// compresses the shared golden images
// (Engine/Source/Developer/TextureTools/Tests/BlockGoldenImages.h) block by block in the order of
// nv::SlowCompressor (nvtt/CompressDXT.cpp). The output file holds the NVTT single color tables,
// and for every image and format the input hash, the hash of the NVTT decode, the hash of the
// compressed bytes and (except for the large hash-only images) the compressed bytes. NVTT sources
// are not copied into the repository: make_golden.py passes the UE clone include paths to the
// compiler.
//
// Usage: NvttGoldenProbe <output file>

#define NV_NO_ASSERT 1

#include <nvtt/squish/colourset.cpp>
#include <nvtt/squish/colourfit.cpp>
#include <nvtt/squish/weightedclusterfit.cpp>
#include <nvtt/squish/colourblock.cpp>
#include <nvtt/squish/maths.cpp>
#include <nvimage/ColorBlock.cpp>
#include <nvimage/BlockDXT.cpp>
#include <nvtt/QuickCompressDXT.cpp>
// OptimalCompressDXT.cpp has file-local helpers with the same names as QuickCompressDXT.cpp, so it
// builds as the second translation unit (NvttGoldenOptimal.cpp). The probe only needs its single
// color tables here.
#include <nvtt/SingleColorLookup.h>

#include "../../Engine/Source/Developer/TextureTools/Tests/BlockGoldenImages.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(__SSE2__) || defined(__AVX2__) || defined(__FMA__)
#error                                                                                             \
    "The probe must build the squish SSE2 path without AVX2 or FMA (the UE non-AVX2 DLL settings)."
#endif

// nv::Image storage functions used by ColorBlock::init (nvimage/Image.cpp semantics). Image.cpp
// itself pulls in the image file readers, so the probe defines the few functions it needs.
namespace nv {

// nvcore/Memory.cpp: NVTT replaces the global new and delete with these.
void* mem::malloc(size_t size) {
    return ::malloc(size);
}

void* mem::malloc(size_t size, const char*, int) {
    return ::malloc(size);
}

void mem::free(const void* ptr) {
    ::free(const_cast<void*>(ptr));
}

void* mem::realloc(void* ptr, size_t size) {
    return ::realloc(ptr, size);
}

Image::Image() : m_width(0), m_height(0), m_format(Format_RGB), m_data(NULL) {
}

Image::~Image() {
}

void Image::wrap(void* data, uint w, uint h) {
    m_data = (Color32*)data;
    m_width = w;
    m_height = h;
}

uint Image::width() const {
    return m_width;
}

uint Image::height() const {
    return m_height;
}

const Color32& Image::pixel(uint idx) const {
    return m_data[idx];
}

Color32& Image::pixel(uint idx) {
    return m_data[idx];
}

} // namespace nv

namespace {

namespace golden = rocks::test::block_golden;

// Formats in golden file order (TextureBlockFormat Bc1, Bc3, Bc4, Bc5).
enum Format : unsigned {
    Bc1,
    Bc3,
    Bc4,
    Bc5,
    FormatCount
};

constexpr unsigned BlockBytes[FormatCount] = {8, 16, 8, 16};

struct Writer {
    std::vector<unsigned char> bytes;

    void U8(unsigned value) {
        bytes.push_back(static_cast<unsigned char>(value));
    }

    void U32(unsigned value) {
        for (int shift = 0; shift < 32; shift += 8) {
            U8((value >> shift) & 0xFF);
        }
    }

    void U64(unsigned long long value) {
        for (int shift = 0; shift < 64; shift += 8) {
            U8(static_cast<unsigned>((value >> shift) & 0xFF));
        }
    }

    void Bytes(const void* data, size_t count) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bytes.insert(bytes.end(), p, p + count);
    }
};

// One block decoded into RGBA8 in the Rocks reference decoder layout (BC4: R 0 0 255, BC5: R G 0
// 255).
void DecodeBlock(Format format, const unsigned char* block, unsigned char out[64]) {
    nv::ColorBlock decoded;
    nv::ColorBlock second;
    switch (format) {
    case Bc1:
        reinterpret_cast<const nv::BlockDXT1*>(block)->decodeBlock(&decoded);
        break;
    case Bc3:
        reinterpret_cast<const nv::BlockDXT5*>(block)->decodeBlock(&decoded);
        break;
    case Bc4:
        reinterpret_cast<const nv::AlphaBlockDXT5*>(block)->decodeBlock(&decoded);
        break;
    case Bc5:
        reinterpret_cast<const nv::AlphaBlockDXT5*>(block)->decodeBlock(&decoded);
        reinterpret_cast<const nv::AlphaBlockDXT5*>(block + 8)->decodeBlock(&second);
        break;
    default:
        break;
    }

    for (unsigned i = 0; i < 16; ++i) {
        const nv::Color32 c = decoded.color(i);
        unsigned char* texel = out + i * 4;
        switch (format) {
        case Bc1:
        case Bc3:
            texel[0] = c.r;
            texel[1] = c.g;
            texel[2] = c.b;
            texel[3] = c.a;
            break;
        case Bc4:
            texel[0] = c.a;
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = 255;
            break;
        case Bc5:
            texel[0] = c.a;
            texel[1] = second.color(i).a;
            texel[2] = 0;
            texel[3] = 255;
            break;
        default:
            break;
        }
    }
}

// Compresses one block the way nv::SlowCompressor does for Quality_Production (UE TextureFormatDXT
// settings: color weights 1, 1, 1, no DXT1a, no CUDA).
void CompressBlock(Format format, const nv::Image& image, unsigned x, unsigned y,
                   squish::WeightedClusterFit& fit, unsigned char* out) {
    nv::ColorBlock rgba;
    rgba.init(&image, x, y);
    switch (format) {
    case Bc1: {
        nv::BlockDXT1 block;
        if (rgba.isSingleColor()) {
            nv::OptimalCompress::compressDXT1(rgba.color(0), &block);
        } else {
            squish::ColourSet colours((uint8*)rgba.colors(), 0, true);
            fit.SetColourSet(&colours, squish::kDxt1);
            fit.Compress(&block);
        }

        std::memcpy(out, &block, sizeof(block));
        break;
    }
    case Bc3: {
        nv::BlockDXT5 block;
        nv::QuickCompress::compressDXT5A(rgba, &block.alpha);
        if (rgba.isSingleColor()) {
            nv::OptimalCompress::compressDXT1(rgba.color(0), &block.color);
        } else {
            squish::ColourSet colours((uint8*)rgba.colors(), squish::kWeightColourByAlpha);
            fit.SetColourSet(&colours, 0);
            fit.Compress(&block.color);
        }

        std::memcpy(out, &block, sizeof(block));
        break;
    }
    case Bc4: {
        // UE compresses BC4 only for the Alpha setting after ReplicateRed (A = R), and
        // nv::SlowCompressor::compressBC4 compresses the alpha channel: splatX gives the same
        // input.
        nv::AlphaBlockDXT5 block;
        rgba.splatX();
        nv::QuickCompress::compressDXT5A(rgba, &block);
        std::memcpy(out, &block, sizeof(block));
        break;
    }
    case Bc5: {
        nv::ColorBlock ycolor = rgba;
        rgba.splatX();
        ycolor.splatY();
        nv::BlockATI2 block;
        nv::QuickCompress::compressDXT5A(rgba, &block.x);
        nv::QuickCompress::compressDXT5A(ycolor, &block.y);
        std::memcpy(out, &block, sizeof(block));
        break;
    }
    default:
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: NvttGoldenProbe <output file>\n");
        return 2;
    }

    static_assert(sizeof(nv::BlockDXT1) == 8 && sizeof(nv::BlockDXT5) == 16 &&
                      sizeof(nv::AlphaBlockDXT5) == 8 && sizeof(nv::BlockATI2) == 16,
                  "unexpected NVTT block sizes");
    constexpr unsigned GoldenVersion = 2;
    constexpr unsigned CaseCount = sizeof(golden::Cases) / sizeof(golden::Cases[0]);
    Writer writer;
    writer.Bytes("RNVG", 4);
    writer.U32(GoldenVersion);
    writer.U32(CaseCount * FormatCount);
    // Single color tables (OMatch5, OMatch6 — nvtt/SingleColorLookup.h).
    writer.Bytes(OMatch5, sizeof(OMatch5));
    writer.Bytes(OMatch6, sizeof(OMatch6));

    squish::WeightedClusterFit fit;
    fit.SetMetric(1.0f, 1.0f, 1.0f);
    std::vector<unsigned char> rgba(golden::MaxTexels * 4);
    std::vector<nv::Color32> pixels(golden::MaxTexels);
    for (const golden::Case& one : golden::Cases) {
        golden::Generate(one, rgba.data());
        const unsigned texels = one.sizeX * one.sizeY;
        const unsigned long long inputHash = golden::Fnv1a64(rgba.data(), texels * 4ull);
        for (unsigned i = 0; i < texels; ++i) {
            pixels[i] =
                nv::Color32(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
        }

        nv::Image image;
        image.wrap(pixels.data(), one.sizeX, one.sizeY);
        for (unsigned format = 0; format < FormatCount; ++format) {
            const unsigned blocksX = (one.sizeX + 3) / 4;
            const unsigned blocksY = (one.sizeY + 3) / 4;
            std::vector<unsigned char> output(blocksX * blocksY * BlockBytes[format]);
            unsigned long long decodeHash = golden::FnvOffset;
            unsigned char* cursor = output.data();
            for (unsigned y = 0; y < one.sizeY; y += 4) {
                for (unsigned x = 0; x < one.sizeX; x += 4) {
                    CompressBlock(static_cast<Format>(format), image, x, y, fit, cursor);
                    unsigned char decoded[64];
                    DecodeBlock(static_cast<Format>(format), cursor, decoded);
                    decodeHash = golden::Fnv1a64(decoded, sizeof(decoded), decodeHash);
                    cursor += BlockBytes[format];
                }
            }

            const size_t nameBytes = std::strlen(one.name);
            writer.U32(static_cast<unsigned>(nameBytes));
            writer.Bytes(one.name, nameBytes);
            writer.U32(format);
            writer.U32(one.sizeX);
            writer.U32(one.sizeY);
            writer.U64(inputHash);
            writer.U64(decodeHash);
            writer.U64(golden::Fnv1a64(output.data(), output.size()));
            // Large images keep only the hashes (output byte count 0).
            const size_t stored = one.hashOnly ? 0 : output.size();
            writer.U32(static_cast<unsigned>(stored));
            writer.Bytes(output.data(), stored);
        }

        image.wrap(NULL, 0, 0);
    }

    FILE* file = std::fopen(argv[1], "wb");
    if (file == NULL) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    const size_t written = std::fwrite(writer.bytes.data(), 1, writer.bytes.size(), file);
    std::fclose(file);
    if (written != writer.bytes.size()) {
        std::fprintf(stderr, "write failed\n");
        return 1;
    }

    std::printf("wrote %zu bytes, %u entries\n", writer.bytes.size(), CaseCount * FormatCount);
    return 0;
}
