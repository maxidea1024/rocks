#include "BlockGoldenImages.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureBlockEncode.h>

#include <stdio.h>
#include <string.h>

// BC1·BC3·BC4·BC5 블록 인코더 시험이다(Docs/Design/AssetSystem 12.17.4 4). golden은 NVTT 2.0.8을 UE
// 비 AVX2 DLL 설정으로 빌드한 probe가 만들었다(Tools/NvttGolden). 판정은 모든 블록의 byte 동일이다.
using namespace rocks;
namespace texture_block_encode_tests_file {

namespace {

namespace golden = test::block_golden;

constexpr usize PathCapacity = 4096;
// golden 파일 byte 상한이다(지금 약 190 KiB).
constexpr uint64 MaxGoldenBytes = uint64{4} << 20;
constexpr StringView GoldenPath =
    "/Engine/Source/Developer/TextureTools/Tests/Fixtures/NvttGolden/NvttBlockGolden.bin";
constexpr uint32 GoldenVersion = 2;
constexpr uint32 FormatCount = 4;
constexpr usize TableBytes = 256 * 2;

constexpr TextureBlockFormat Formats[FormatCount] = {
    TextureBlockFormat::Bc1, TextureBlockFormat::Bc3, TextureBlockFormat::Bc4,
    TextureBlockFormat::Bc5};

bool IsSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

// 실행 파일 위치에서 위로 올라가며 저장소의 golden 파일을 찾는다.
bool FindGolden(FixedStringBuilder& out) noexcept {
    char executable[PathCapacity];
    usize length = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), length)) {
        return false;
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    while (length != 0) {
        usize cut = length;
        while (cut != 0 && !IsSeparator(executable[cut - 1])) {
            --cut;
        }

        if (cut == 0) {
            return false;
        }

        length = cut - 1;
        out.Clear();
        if (!out.TryAppendFormat("{}{}", StringView(executable, length), GoldenPath)) {
            return false;
        }

        platform::FileInfo info;
        if (files.Stat(out.View().Data(), out.View().ByteCount(), info) &&
            info.type == platform::FileType::File) {
            return true;
        }
    }

    return false;
}

// golden 파일 읽기 커서다(리틀 엔디언).
struct Reader {
    Span<const uint8> bytes;
    usize at = 0;
    bool ok = true;

    bool Has(usize count) noexcept {
        ok = ok && bytes.Size() - at >= count && at <= bytes.Size();
        return ok;
    }

    uint32 U32() noexcept {
        if (!Has(4)) {
            return 0;
        }

        uint32 value = 0;
        for (uint32 i = 0; i < 4; ++i) {
            value |= uint32{bytes[at + i]} << (8 * i);
        }

        at += 4;
        return value;
    }

    uint64 U64() noexcept {
        const uint64 low = U32();
        const uint64 high = U32();
        return low | (high << 32);
    }

    Span<const uint8> Take(usize count) noexcept {
        if (!Has(count)) {
            return {};
        }

        const Span<const uint8> part(bytes.Data() + at, count);
        at += count;
        return part;
    }
};

struct GoldenEntry {
    StringView name;
    uint32 format = 0;
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint64 inputHash = 0;
    uint64 decodeHash = 0;
    uint64 outputHash = 0;
    // 해시 전용 항목(큰 그림)은 비어 있다.
    Span<const uint8> output;
};

struct Golden {
    explicit Golden(Allocator& allocator) noexcept : file(allocator), entries(allocator) {
    }

    Array<uint8> file;
    Span<const uint8> table5;
    Span<const uint8> table6;
    Array<GoldenEntry> entries;
};

bool LoadGolden(Golden& golden) noexcept {
    char pathText[PathCapacity];
    FixedStringBuilder path{Span<char>(pathText)};
    if (!FindGolden(path)) {
        return false;
    }

    bool existed = false;
    if (!ReadWholeFile(platform::PhysicalFileSystem(), path.View(), MaxGoldenBytes, golden.file,
                       existed) ||
        !existed) {
        return false;
    }

    Reader reader{golden.file.View()};
    const Span<const uint8> magic = reader.Take(4);
    if (!reader.ok || memcmp(magic.Data(), "RNVG", 4) != 0 || reader.U32() != GoldenVersion) {
        return false;
    }

    const uint32 count = reader.U32();
    golden.table5 = reader.Take(TableBytes);
    golden.table6 = reader.Take(TableBytes);
    for (uint32 index = 0; index < count && reader.ok; ++index) {
        GoldenEntry entry;
        const uint32 nameBytes = reader.U32();
        const Span<const uint8> name = reader.Take(nameBytes);
        entry.name = StringView(reinterpret_cast<const char*>(name.Data()), name.Size());
        entry.format = reader.U32();
        entry.sizeX = reader.U32();
        entry.sizeY = reader.U32();
        entry.inputHash = reader.U64();
        entry.decodeHash = reader.U64();
        entry.outputHash = reader.U64();
        entry.output = reader.Take(reader.U32());
        if (!reader.ok || !golden.entries.TryPushBack(entry)) {
            return false;
        }
    }

    return reader.ok && reader.at == golden.file.Size();
}

const golden::Case* FindCase(StringView name) noexcept {
    for (const golden::Case& one : golden::Cases) {
        if (StringView(one.name) == name) {
            return &one;
        }
    }

    return nullptr;
}

// 생성기 그림을 RGBA8 Array로 만든다.
bool Generate(Allocator& allocator, const golden::Case& one, Array<uint8>& out) noexcept {
    if (!out.TryResize(usize{one.sizeX} * one.sizeY * TextureBlockTexelBytes)) {
        return false;
    }

    golden::Generate(one, out.Data());
    (void)allocator;
    return true;
}

// SingleColorLookup.h 주석의 PrepareOptTable 식이다(시험이 따로 다시 만든다). 보간은 주석 본문의
// Mul8Bit가 아니라 같은 주석의 Lerp13((2·max + min) / 3)이다. NVTT 표가 이 식의 값이다(12.17.4 1).
int Lerp13(int from, int to) noexcept {
    return (from * 2 + to) / 3;
}

void PrepareOptTable(uint8 (&table)[TableBytes], const uint8* expand, int size) noexcept {
    for (int i = 0; i < 256; ++i) {
        float bestErr = 256;
        for (int min = 0; min < size; ++min) {
            for (int max = 0; max < size; ++max) {
                const int mine = expand[min];
                const int maxe = expand[max];
                const int diff = Lerp13(maxe, mine) - i;
                float err = static_cast<float>(diff < 0 ? -diff : diff);
                const int span = max - min;
                const float spread = 0.03f * static_cast<float>(span < 0 ? -span : span);
                err += spread;
                if (err < bestErr) {
                    table[i * 2 + 0] = static_cast<uint8>(max);
                    table[i * 2 + 1] = static_cast<uint8>(min);
                    bestErr = err;
                }
            }
        }
    }
}

uint32 ColorEndpoint0(const uint8* block) noexcept {
    return uint32{block[0]} | (uint32{block[1]} << 8);
}

uint32 ColorEndpoint1(const uint8* block) noexcept {
    return uint32{block[2]} | (uint32{block[3]} << 8);
}

uint32 ColorIndices(const uint8* block) noexcept {
    return uint32{block[4]} | (uint32{block[5]} << 8) | (uint32{block[6]} << 16) |
           (uint32{block[7]} << 24);
}

// golden 그림이 드문 경로를 실제로 지나는지 센다(12.17.4 4 — 경로가 없으면 규칙 시험이 빈 채로
// 통과한다).
struct PathCoverage {
    // BC1에서 3색 모드가 이긴 블록(col0 < col1)이다.
    usize bc1ThreeColor = 0;
    // BC3 색에서 4색 같은 끝점 블록(index 모두 0)이다.
    usize fourColorEqual = 0;
    // 알파 최소제곱이 채택되어 끝점이 처음 inset 끝점과 다른 블록이다.
    usize alphaRefined = 0;
    // 6값 모드에서 index 6·7(0·255)을 쓰는 알파 블록이다(보고만 한다).
    usize alphaSixValueExtremes = 0;
};

uint32 AlphaIndex(const uint8* block, uint32 texel) noexcept {
    uint64 bits = 0;
    for (uint32 i = 0; i < 8; ++i) {
        bits |= uint64{block[i]} << (8 * i);
    }

    return static_cast<uint32>((bits >> (16 + 3 * texel)) & 0x7);
}

// 알파 블록 하나를 센다. values는 그 블록 입력 채널 16개다.
void CountAlpha(const uint8* block, const uint8 (&values)[TextureBlockTexels],
                PathCoverage& coverage) noexcept {
    uint32 high = 0;
    uint32 low = 255;
    for (const uint8 value : values) {
        high = value > high ? value : high;
        low = value < low ? value : low;
    }

    const uint32 inset = (high - low) / 34;
    if (block[0] != high - inset || block[1] != low + inset) {
        ++coverage.alphaRefined;
    }

    if (block[0] <= block[1]) {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            if (AlphaIndex(block, i) >= 6) {
                ++coverage.alphaSixValueExtremes;
                break;
            }
        }
    }
}

// 4의 배수 크기 그림의 출력 블록을 센다. 4색 끝점 규칙(col0 ≥ col1, 같으면 index 0 또는 단색 index
// 2)도 블록마다 단언한다.
void CountPaths(TextureBlockFormat format, const uint8* input, uint32 sizeX, uint32 sizeY,
                const uint8* output, PathCoverage& coverage) noexcept {
    if (sizeX % 4 != 0 || sizeY % 4 != 0) {
        return;
    }

    const uint32 blockBytes = TextureBlockBytes(format);
    const uint32 blocksX = sizeX / 4;
    for (uint32 by = 0; by < sizeY / 4; ++by) {
        for (uint32 bx = 0; bx < blocksX; ++bx) {
            const uint8* block = output + (by * blocksX + bx) * blockBytes;
            uint8 alpha[TextureBlockTexels];
            uint8 red[TextureBlockTexels];
            for (uint32 i = 0; i < TextureBlockTexels; ++i) {
                const uint8* texel = input + ((by * 4 + i / 4) * sizeX + bx * 4 + i % 4) * 4;
                alpha[i] = texel[3];
                red[i] = texel[0];
            }

            if (format == TextureBlockFormat::Bc1) {
                coverage.bc1ThreeColor += ColorEndpoint0(block) < ColorEndpoint1(block) ? 1 : 0;
            } else if (format == TextureBlockFormat::Bc3) {
                const uint8* colour = block + 8;
                CHECK(ColorEndpoint0(colour) >= ColorEndpoint1(colour));
                if (ColorEndpoint0(colour) == ColorEndpoint1(colour)) {
                    const uint32 indices = ColorIndices(colour);
                    CHECK(indices == 0 || indices == 0xAAAAAAAAu);
                    coverage.fourColorEqual += indices == 0 ? 1 : 0;
                }

                CountAlpha(block, alpha, coverage);
            } else if (format == TextureBlockFormat::Bc4) {
                CountAlpha(block, red, coverage);
            }
        }
    }
}

void Fill(uint8 (&texels)[TextureBlockRgbaBytes], uint8 r, uint8 g, uint8 b, uint8 a) noexcept {
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        texels[i * 4 + 0] = r;
        texels[i * 4 + 1] = g;
        texels[i * 4 + 2] = b;
        texels[i * 4 + 3] = a;
    }
}

} // namespace

// golden: 모든 그림·형식이 NVTT와 byte로 같고(다르면 다른 블록 수·첫 위치 보고 — 해시 전용 항목은
// 출력 해시), 기준 디코더 결과가 NVTT 디코드와 같다. 입력 해시가 다르면 생성기가 probe와 어긋난
// 것이다. 드문 경로(3색 승리·4색 같은 끝점·알파 최소제곱 채택)를 실제로 지나는지도 센다.
TEST(TextureBlockEncode_MatchesNvttGolden) {
    SystemAllocator allocator;
    Golden golden(allocator);
    REQUIRE(LoadGolden(golden));
    constexpr usize CaseCount = sizeof(golden::Cases) / sizeof(golden::Cases[0]);
    CHECK(golden.entries.Size() == CaseCount * FormatCount);
    usize checkedBlocks = 0;
    PathCoverage coverage;
    for (const GoldenEntry& entry : golden.entries) {
        const golden::Case* one = FindCase(entry.name);
        REQUIRE(one != nullptr);
        REQUIRE(entry.format < FormatCount);
        CHECK(entry.sizeX == one->sizeX && entry.sizeY == one->sizeY);
        Array<uint8> input(allocator);
        REQUIRE(Generate(allocator, *one, input));
        CHECK(golden::Fnv1a64(input.Data(), input.Size()) == entry.inputHash);
        const TextureBlockFormat format = Formats[entry.format];
        auto bytes = TextureBlockOutputBytes(format, one->sizeX, one->sizeY, 1);
        REQUIRE(bytes);
        REQUIRE(one->hashOnly ? entry.output.IsEmpty() : bytes.Value() == entry.output.Size());
        Array<uint8> output(allocator);
        REQUIRE(output.TryResize(static_cast<usize>(bytes.Value())));
        TextureBlockImage image;
        image.rgba = input.View();
        image.sizeX = one->sizeX;
        image.sizeY = one->sizeY;
        REQUIRE(EncodeTextureBlocks(format, image, output.View()));
        const uint32 blockBytes = TextureBlockBytes(format);
        const usize blocks = output.Size() / blockBytes;
        usize different = 0;
        usize first = NumericLimits<usize>::Max();
        uint64 decodeHash = golden::FnvOffset;
        for (usize block = 0; block < blocks; ++block) {
            const uint8* mine = output.Data() + block * blockBytes;
            if (!one->hashOnly &&
                memcmp(mine, entry.output.Data() + block * blockBytes, blockBytes) != 0) {
                ++different;
                first = first < block ? first : block;
            }

            // 해시 전용 항목은 자기 출력을 푼다(출력 해시가 같으면 NVTT 출력과 같다).
            const uint8* source = one->hashOnly ? mine : entry.output.Data() + block * blockBytes;
            uint8 decoded[TextureBlockRgbaBytes];
            DecodeTextureBlock(format, source, decoded);
            decodeHash = golden::Fnv1a64(decoded, sizeof(decoded), decodeHash);
        }

        CHECK(golden::Fnv1a64(output.Data(), output.Size()) == entry.outputHash);
        CountPaths(format, input.Data(), one->sizeX, one->sizeY, output.Data(), coverage);

        if (different != 0) {
            printf("  %.*s format %u: %zu of %zu blocks differ (first %zu)\n",
                   static_cast<int>(entry.name.ByteCount()), entry.name.Data(), entry.format,
                   different, blocks, first);
        }

        CHECK(different == 0);
        CHECK(decodeHash == entry.decodeHash);
        checkedBlocks += blocks;
    }

    CHECK(checkedBlocks > 0);
    printf("  paths: bc1 three-color %zu, four-color equal %zu, alpha refined %zu, six-value 0/255 "
           "%zu\n",
           coverage.bc1ThreeColor, coverage.fourColorEqual, coverage.alphaRefined,
           coverage.alphaSixValueExtremes);
    CHECK(coverage.bc1ThreeColor > 0);
    CHECK(coverage.fourColorEqual > 0);
    CHECK(coverage.alphaRefined > 0);
}

// 단색 표: PrepareOptTable 식(Lerp13 보간)으로 다시 만든 표가 NVTT 표와 같고, 인코더의 단색 블록
// 끝점이 그 표의 값이다(index 2, col0 < col1이면 맞바꾸고 index 3, 같으면 index 2 그대로 — 12.17절
// TD2).
TEST(TextureBlockEncode_SingleColorTable) {
    SystemAllocator allocator;
    Golden golden(allocator);
    REQUIRE(LoadGolden(golden));
    uint8 expand5[32];
    uint8 expand6[64];
    for (int i = 0; i < 32; ++i) {
        expand5[i] = static_cast<uint8>((i << 3) | (i >> 2));
    }

    for (int i = 0; i < 64; ++i) {
        expand6[i] = static_cast<uint8>((i << 2) | (i >> 4));
    }

    uint8 table5[TableBytes] = {};
    uint8 table6[TableBytes] = {};
    PrepareOptTable(table5, expand5, 32);
    PrepareOptTable(table6, expand6, 64);
    CHECK(memcmp(table5, golden.table5.Data(), TableBytes) == 0);
    CHECK(memcmp(table6, golden.table6.Data(), TableBytes) == 0);
    uint32 swapped = 0;
    uint32 equal = 0;
    for (uint32 v = 0; v < 256; ++v) {
        uint8 texels[TextureBlockRgbaBytes];
        Fill(texels, static_cast<uint8>(v), static_cast<uint8>(255 - v), static_cast<uint8>(v / 2),
             static_cast<uint8>(v));
        uint8 block[8];
        EncodeTextureBlock(TextureBlockFormat::Bc1, texels, block);
        const uint32 col0 = (uint32{table5[v * 2]} << 11) | (uint32{table6[(255 - v) * 2]} << 5) |
                            table5[(v / 2) * 2];
        const uint32 col1 = (uint32{table5[v * 2 + 1]} << 11) |
                            (uint32{table6[(255 - v) * 2 + 1]} << 5) | table5[(v / 2) * 2 + 1];
        if (col0 < col1) {
            ++swapped;
            CHECK(ColorEndpoint0(block) == col1 && ColorEndpoint1(block) == col0 &&
                  ColorIndices(block) == 0xFFFFFFFFu);
        } else {
            equal += col0 == col1 ? 1 : 0;
            CHECK(ColorEndpoint0(block) == col0 && ColorEndpoint1(block) == col1 &&
                  ColorIndices(block) == 0xAAAAAAAAu);
        }

        // BC3 색 블록도 같은 단색 경로다(알파는 보지 않는다).
        uint8 bc3[16];
        EncodeTextureBlock(TextureBlockFormat::Bc3, texels, bc3);
        CHECK(memcmp(bc3 + 8, block, 8) == 0);
    }

    CHECK(swapped > 0 && equal > 0);
}

// 끝점 규칙(12.17절 TD2 F6): 알파 블록은 값이 모두 같으면 alpha0 = alpha1(6값 모드, index 0),
// 범위가 있으면 처음 끝점이 alpha0 > alpha1(8값 모드)이다. BC4는 R, BC5는 R·G 블록을 따로 만들고
// BC3 알파 블록과 같은 8 byte다. 4색 끝점 규칙(col0 ≥ col1, 같으면 index 0)은 golden 시험이
// 블록마다 본다.
TEST(TextureBlockEncode_EndpointRules) {
    uint8 texels[TextureBlockRgbaBytes];
    uint8 bc3[16];
    // 상수 알파: 6값 모드 끝점(같은 값)과 index 0.
    Fill(texels, 10, 20, 30, 77);
    EncodeTextureBlock(TextureBlockFormat::Bc3, texels, bc3);
    CHECK(bc3[0] == 77 && bc3[1] == 77);
    for (uint32 i = 2; i < 8; ++i) {
        CHECK(bc3[i] == 0);
    }

    // 알파 범위가 있으면 8값 모드(alpha0 > alpha1).
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        texels[i * 4 + 3] = static_cast<uint8>(i * 16);
        texels[i * 4 + 0] = static_cast<uint8>(255 - i * 16);
        texels[i * 4 + 1] = static_cast<uint8>(i * 8);
    }

    EncodeTextureBlock(TextureBlockFormat::Bc3, texels, bc3);
    CHECK(bc3[0] > bc3[1]);
    // BC4(R)·BC5(R, G)는 같은 채널 값의 BC3 알파 블록과 같다.
    uint8 alphaFromRed[TextureBlockRgbaBytes];
    uint8 alphaFromGreen[TextureBlockRgbaBytes];
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        for (uint32 k = 0; k < 4; ++k) {
            alphaFromRed[i * 4 + k] = texels[i * 4 + k];
            alphaFromGreen[i * 4 + k] = texels[i * 4 + k];
        }

        alphaFromRed[i * 4 + 3] = texels[i * 4 + 0];
        alphaFromGreen[i * 4 + 3] = texels[i * 4 + 1];
    }

    uint8 red[16];
    uint8 green[16];
    EncodeTextureBlock(TextureBlockFormat::Bc3, alphaFromRed, red);
    EncodeTextureBlock(TextureBlockFormat::Bc3, alphaFromGreen, green);
    uint8 bc4[8];
    uint8 bc5[16];
    EncodeTextureBlock(TextureBlockFormat::Bc4, texels, bc4);
    EncodeTextureBlock(TextureBlockFormat::Bc5, texels, bc5);
    CHECK(memcmp(bc4, red, 8) == 0);
    CHECK(memcmp(bc5, red, 8) == 0 && memcmp(bc5 + 8, green, 8) == 0);
    // BC1은 3색 모드를 고를 수 있지만 index 3(투명 검정)은 쓰지 않는다.
    for (const golden::Case& one : golden::Cases) {
        if (one.sizeX != 32) {
            continue;
        }

        SystemAllocator allocator;
        Array<uint8> input(allocator);
        REQUIRE(Generate(allocator, one, input));
        Array<uint8> output(allocator);
        REQUIRE(output.TryResize(8 * 8 * 8));
        TextureBlockImage image;
        image.rgba = input.View();
        image.sizeX = one.sizeX;
        image.sizeY = one.sizeY;
        REQUIRE(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, output.View()));
        for (usize block = 0; block < 64; ++block) {
            const uint8* bytes = output.Data() + block * 8;
            if (ColorEndpoint0(bytes) <= ColorEndpoint1(bytes)) {
                const uint32 indices = ColorIndices(bytes);
                for (uint32 i = 0; i < TextureBlockTexels; ++i) {
                    CHECK(((indices >> (2 * i)) & 3) != 3);
                }
            }
        }
    }
}

// 가장자리·pitch·slice: 1–9 크기는 NVTT remainder 채움(0,0,0,0 / 0,1,0,1 / 0,1,2,0)과 같고(golden
// edge 그림), 블록 하나를 직접 채운 결과와 같다. pitch·slice 입력은 빽빽한 입력과 같은 byte다.
TEST(TextureBlockEncode_EdgesPitchAndSlices) {
    SystemAllocator allocator;
    constexpr uint32 Remainder[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 0}, {0, 1, 2, 3}};
    for (uint32 w = 1; w <= 9; ++w) {
        for (uint32 h = 1; h <= 9; ++h) {
            const golden::Case one{"edge", golden::WhiteNoise, w, h, 100 + w * 10 + h};
            Array<uint8> input(allocator);
            REQUIRE(Generate(allocator, one, input));
            for (const TextureBlockFormat format : Formats) {
                auto bytes = TextureBlockOutputBytes(format, w, h, 1);
                REQUIRE(bytes);
                const uint32 blocksX = (w + 3) / 4;
                const uint32 blocksY = (h + 3) / 4;
                CHECK(bytes.Value() == uint64{blocksX} * blocksY * TextureBlockBytes(format));
                Array<uint8> output(allocator);
                REQUIRE(output.TryResize(static_cast<usize>(bytes.Value())));
                TextureBlockImage image;
                image.rgba = input.View();
                image.sizeX = w;
                image.sizeY = h;
                REQUIRE(EncodeTextureBlocks(format, image, output.View()));
                // 마지막 블록을 직접 채워 인코딩한 값과 같다.
                const uint32 bx = (blocksX - 1) * 4;
                const uint32 by = (blocksY - 1) * 4;
                const uint32 bw = w - bx < 4 ? w - bx : 4;
                const uint32 bh = h - by < 4 ? h - by : 4;
                uint8 texels[TextureBlockRgbaBytes];
                for (uint32 row = 0; row < 4; ++row) {
                    for (uint32 column = 0; column < 4; ++column) {
                        const uint32 x = bx + Remainder[bw - 1][column];
                        const uint32 y = by + Remainder[bh - 1][row];
                        memcpy(texels + (row * 4 + column) * 4, input.Data() + (y * w + x) * 4, 4);
                    }
                }

                uint8 expected[16];
                EncodeTextureBlock(format, texels, expected);
                const uint32 blockBytes = TextureBlockBytes(format);
                CHECK(memcmp(output.Data() + output.Size() - blockBytes, expected, blockBytes) ==
                      0);
            }
        }
    }

    // pitch·slice: 행 사이 여백 12 byte, slice 사이 여백 64 byte, slice 셋(서로 다른 그림).
    constexpr uint32 SizeX = 10;
    constexpr uint32 SizeY = 7;
    constexpr uint32 Slices = 3;
    constexpr usize RowPitch = SizeX * 4 + 12;
    constexpr usize SlicePitch = RowPitch * SizeY + 64;
    Array<uint8> padded(allocator);
    REQUIRE(padded.TryResize(SlicePitch * Slices));
    Array<uint8> dense(allocator);
    REQUIRE(dense.TryResize(usize{SizeX} * SizeY * 4 * Slices));
    for (uint32 slice = 0; slice < Slices; ++slice) {
        const golden::Case one{"slice", golden::WhiteNoise, SizeX, SizeY, 500 + slice};
        Array<uint8> input(allocator);
        REQUIRE(Generate(allocator, one, input));
        for (uint32 y = 0; y < SizeY; ++y) {
            memcpy(padded.Data() + slice * SlicePitch + y * RowPitch, input.Data() + y * SizeX * 4,
                   SizeX * 4);
            memcpy(dense.Data() + (slice * SizeY + y) * SizeX * 4, input.Data() + y * SizeX * 4,
                   SizeX * 4);
        }
    }

    for (const TextureBlockFormat format : Formats) {
        auto bytes = TextureBlockOutputBytes(format, SizeX, SizeY, Slices);
        REQUIRE(bytes);
        Array<uint8> fromPadded(allocator);
        Array<uint8> fromDense(allocator);
        REQUIRE(fromPadded.TryResize(static_cast<usize>(bytes.Value())));
        REQUIRE(fromDense.TryResize(static_cast<usize>(bytes.Value())));
        TextureBlockImage image;
        image.rgba = padded.View();
        image.sizeX = SizeX;
        image.sizeY = SizeY;
        image.numSlices = Slices;
        image.rowPitch = RowPitch;
        image.slicePitch = SlicePitch;
        REQUIRE(EncodeTextureBlocks(format, image, fromPadded.View()));
        image.rgba = dense.View();
        image.rowPitch = 0;
        image.slicePitch = 0;
        REQUIRE(EncodeTextureBlocks(format, image, fromDense.View()));
        CHECK(fromPadded.Size() == fromDense.Size() &&
              memcmp(fromPadded.Data(), fromDense.Data(), fromDense.Size()) == 0);
        // 마지막 slice·행의 pitch 여백은 없어도 된다.
        image.rgba = Span<const uint8>(padded.Data(), SlicePitch * (Slices - 1) +
                                                          RowPitch * (SizeY - 1) + SizeX * 4);
        image.rowPitch = RowPitch;
        image.slicePitch = SlicePitch;
        REQUIRE(EncodeTextureBlocks(format, image, fromPadded.View()));
        CHECK(memcmp(fromPadded.Data(), fromDense.Data(), fromDense.Size()) == 0);
        // slicePitch는 slice가 닿는 byte(rowPitch × (sizeY − 1) + 행 byte) 이상이면 받는다 —
        // rowPitch × sizeY보다 작아도 된다.
        constexpr usize TightSlice = RowPitch * (SizeY - 1) + SizeX * 4;
        Array<uint8> tight(allocator);
        REQUIRE(tight.TryResize(TightSlice * Slices));
        for (uint32 slice = 0; slice < Slices; ++slice) {
            for (uint32 y = 0; y < SizeY; ++y) {
                memcpy(tight.Data() + slice * TightSlice + y * RowPitch,
                       dense.Data() + (slice * SizeY + y) * SizeX * 4, SizeX * 4);
            }
        }

        image.rgba = tight.View();
        image.slicePitch = TightSlice;
        REQUIRE(EncodeTextureBlocks(format, image, fromPadded.View()));
        CHECK(memcmp(fromPadded.Data(), fromDense.Data(), fromDense.Size()) == 0);
        image.slicePitch = TightSlice - 1;
        CHECK(EncodeTextureBlocks(format, image, fromPadded.View()).Error() ==
              ErrorCode::InvalidArgument);
    }
}

// 결정성: 같은 입력을 다른 출력 버퍼에 두 번 인코딩하면 같은 byte다(블록마다 순수 함수).
TEST(TextureBlockEncode_Deterministic) {
    SystemAllocator allocator;
    const golden::Case& one = golden::Cases[sizeof(golden::Cases) / sizeof(golden::Cases[0]) - 1];
    Array<uint8> input(allocator);
    REQUIRE(Generate(allocator, one, input));
    for (const TextureBlockFormat format : Formats) {
        auto bytes = TextureBlockOutputBytes(format, one.sizeX, one.sizeY, 1);
        REQUIRE(bytes);
        Array<uint8> first(allocator);
        Array<uint8> second(allocator);
        REQUIRE(first.TryResize(static_cast<usize>(bytes.Value())));
        REQUIRE(second.TryResize(static_cast<usize>(bytes.Value())));
        memset(second.Data(), 0xCD, second.Size());
        TextureBlockImage image;
        image.rgba = input.View();
        image.sizeX = one.sizeX;
        image.sizeY = one.sizeY;
        REQUIRE(EncodeTextureBlocks(format, image, first.View()));
        REQUIRE(EncodeTextureBlocks(format, image, second.View()));
        CHECK(memcmp(first.Data(), second.Data(), first.Size()) == 0);
    }
}

// 인자 오류: 크기 0(InvalidArgument), 넘치는 출력 크기(CapacityExceeded), 입력 부족·pitch 부족·
// slice pitch 부족·out 크기 다름(InvalidArgument).
TEST(TextureBlockEncode_ArgumentErrors) {
    CHECK(TextureBlockBytes(TextureBlockFormat::Bc1) == 8 &&
          TextureBlockBytes(TextureBlockFormat::Bc3) == 16 &&
          TextureBlockBytes(TextureBlockFormat::Bc4) == 8 &&
          TextureBlockBytes(TextureBlockFormat::Bc5) == 16);
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc1, 0, 4, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc1, 4, 0, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc1, 4, 4, 0).Error() ==
          ErrorCode::InvalidArgument);
    constexpr uint32 Huge = NumericLimits<uint32>::Max();
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc3, Huge, Huge, 1).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc1, Huge, Huge, Huge).Error() ==
          ErrorCode::CapacityExceeded);
    auto largest = TextureBlockOutputBytes(TextureBlockFormat::Bc1, Huge, Huge, 1);
    REQUIRE(largest);
    CHECK(largest.Value() == uint64{1} << 63);

    uint8 pixels[8 * 4 * 4] = {};
    uint8 out[32] = {};
    TextureBlockImage image;
    image.rgba = Span<const uint8>(pixels, sizeof(pixels));
    image.sizeX = 8;
    image.sizeY = 4;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 16)));
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 15)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 17)).Error() ==
          ErrorCode::InvalidArgument);
    image.rgba = Span<const uint8>(pixels, sizeof(pixels) - 1);
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 16)).Error() ==
          ErrorCode::InvalidArgument);
    image.rgba = Span<const uint8>(pixels, sizeof(pixels));
    image.rowPitch = 8 * 4 - 1;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 16)).Error() ==
          ErrorCode::InvalidArgument);
    image.rowPitch = 0;
    image.numSlices = 2;
    image.slicePitch = 8 * 4 * 3;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 32)).Error() ==
          ErrorCode::InvalidArgument);
    image.slicePitch = 0;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 32)).Error() ==
          ErrorCode::InvalidArgument);
    image.numSlices = 1;
    image.sizeX = 0;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 16)).Error() ==
          ErrorCode::InvalidArgument);
    image.sizeX = 8;
    image.rowPitch = NumericLimits<usize>::Max();
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc1, image, Span<uint8>(out, 16)).Error() ==
          ErrorCode::CapacityExceeded);
}

} // namespace texture_block_encode_tests_file
