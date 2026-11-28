#include "ImageFuzz.h"

#include <Core/Hash/Sha256.h>
#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>

// PngSuite 대조 시험이다(Docs/Design/AssetSystem 15절, 14.1.119 후속 3번 — 2017-07-19판, 라이선스
// Fixtures/PngSuite/PngSuite.LICENSE). 기대값은 외부 디코더 없이 정한다: `x`로 시작하는 파일은 실패
// (InvalidSource/CorruptData), 나머지는 성공하고 크기는 이름 규칙이며, 같은 그림 묶음(Adam7
// 유무· IDAT 쪼개기·zlib 수준·보조 chunk 변형 — 독립 참조 디코더로 찾았다)은 형식·크기·픽셀이 같다.
using namespace rocks;
using namespace rocks::image;
namespace png_suite_tests_file {

using image_codec_test::ConsistentImage;
using image_codec_test::LiveCountingAllocator;

namespace {

// 파일 하나의 기대값이다. width 0은 실패해야 하는 파일이고, group 0은 묶음 없음이다.
struct SuiteFile {
    const char* name;
    uint32 width;
    uint32 height;
    uint32 group;
};

constexpr SuiteFile SuiteFiles[] = {
    {"basi0g01", 32, 32, 1},  {"basi0g02", 32, 32, 2},  {"basi0g04", 32, 32, 3},
    {"basi0g08", 32, 32, 4},  {"basi0g16", 32, 32, 5},  {"basi2c08", 32, 32, 6},
    {"basi2c16", 32, 32, 7},  {"basi3p01", 32, 32, 8},  {"basi3p02", 32, 32, 9},
    {"basi3p04", 32, 32, 10}, {"basi3p08", 32, 32, 11}, {"basi4a08", 32, 32, 12},
    {"basi4a16", 32, 32, 13}, {"basi6a08", 32, 32, 14}, {"basi6a16", 32, 32, 15},
    {"basn0g01", 32, 32, 1},  {"basn0g02", 32, 32, 2},  {"basn0g04", 32, 32, 3},
    {"basn0g08", 32, 32, 4},  {"basn0g16", 32, 32, 5},  {"basn2c08", 32, 32, 6},
    {"basn2c16", 32, 32, 7},  {"basn3p01", 32, 32, 8},  {"basn3p02", 32, 32, 9},
    {"basn3p04", 32, 32, 10}, {"basn3p08", 32, 32, 11}, {"basn4a08", 32, 32, 12},
    {"basn4a16", 32, 32, 13}, {"basn6a08", 32, 32, 14}, {"basn6a16", 32, 32, 15},
    {"bgai4a08", 32, 32, 12}, {"bgai4a16", 32, 32, 13}, {"bgan6a08", 32, 32, 14},
    {"bgan6a16", 32, 32, 15}, {"bgbn4a08", 32, 32, 12}, {"bggn4a16", 32, 32, 13},
    {"bgwn6a08", 32, 32, 14}, {"bgyn6a16", 32, 32, 15}, {"ccwn2c08", 32, 32, 0},
    {"ccwn3p08", 32, 32, 0},  {"cdfn2c08", 8, 32, 0},   {"cdhn2c08", 32, 8, 0},
    {"cdsn2c08", 8, 8, 0},    {"cdun2c08", 32, 32, 0},  {"ch1n3p04", 32, 32, 10},
    {"ch2n3p08", 32, 32, 11}, {"cm0n0g04", 32, 32, 16}, {"cm7n0g04", 32, 32, 16},
    {"cm9n0g04", 32, 32, 16}, {"cs3n2c16", 32, 32, 0},  {"cs3n3p08", 32, 32, 0},
    {"cs5n2c08", 32, 32, 0},  {"cs5n3p08", 32, 32, 0},  {"cs8n2c08", 32, 32, 0},
    {"cs8n3p08", 32, 32, 0},  {"ct0n0g04", 32, 32, 16}, {"ct1n0g04", 32, 32, 16},
    {"cten0g04", 32, 32, 0},  {"ctfn0g04", 32, 32, 0},  {"ctgn0g04", 32, 32, 0},
    {"cthn0g04", 32, 32, 0},  {"ctjn0g04", 32, 32, 0},  {"ctzn0g04", 32, 32, 16},
    {"exif2c08", 32, 32, 0},  {"f00n0g08", 32, 32, 0},  {"f00n2c08", 32, 32, 0},
    {"f01n0g08", 32, 32, 0},  {"f01n2c08", 32, 32, 0},  {"f02n0g08", 32, 32, 0},
    {"f02n2c08", 32, 32, 0},  {"f03n0g08", 32, 32, 0},  {"f03n2c08", 32, 32, 0},
    {"f04n0g08", 32, 32, 0},  {"f04n2c08", 32, 32, 0},  {"f99n0g04", 32, 32, 0},
    {"g03n0g16", 32, 32, 0},  {"g03n2c08", 32, 32, 0},  {"g03n3p04", 32, 32, 0},
    {"g04n0g16", 32, 32, 0},  {"g04n2c08", 32, 32, 0},  {"g04n3p04", 32, 32, 0},
    {"g05n0g16", 32, 32, 0},  {"g05n2c08", 32, 32, 0},  {"g05n3p04", 32, 32, 0},
    {"g07n0g16", 32, 32, 0},  {"g07n2c08", 32, 32, 0},  {"g07n3p04", 32, 32, 0},
    {"g10n0g16", 32, 32, 0},  {"g10n2c08", 32, 32, 0},  {"g10n3p04", 32, 32, 0},
    {"g25n0g16", 32, 32, 0},  {"g25n2c08", 32, 32, 0},  {"g25n3p04", 32, 32, 0},
    {"oi1n0g16", 32, 32, 5},  {"oi1n2c16", 32, 32, 7},  {"oi2n0g16", 32, 32, 5},
    {"oi2n2c16", 32, 32, 7},  {"oi4n0g16", 32, 32, 5},  {"oi4n2c16", 32, 32, 7},
    {"oi9n0g16", 32, 32, 5},  {"oi9n2c16", 32, 32, 7},  {"PngSuite", 256, 256, 0},
    {"pp0n2c16", 32, 32, 7},  {"pp0n6a08", 32, 32, 0},  {"ps1n0g08", 32, 32, 4},
    {"ps1n2c16", 32, 32, 7},  {"ps2n0g08", 32, 32, 4},  {"ps2n2c16", 32, 32, 7},
    {"s01i3p01", 1, 1, 17},   {"s01n3p01", 1, 1, 17},   {"s02i3p01", 2, 2, 18},
    {"s02n3p01", 2, 2, 18},   {"s03i3p01", 3, 3, 19},   {"s03n3p01", 3, 3, 19},
    {"s04i3p01", 4, 4, 20},   {"s04n3p01", 4, 4, 20},   {"s05i3p02", 5, 5, 21},
    {"s05n3p02", 5, 5, 21},   {"s06i3p02", 6, 6, 22},   {"s06n3p02", 6, 6, 22},
    {"s07i3p02", 7, 7, 23},   {"s07n3p02", 7, 7, 23},   {"s08i3p02", 8, 8, 24},
    {"s08n3p02", 8, 8, 24},   {"s09i3p02", 9, 9, 25},   {"s09n3p02", 9, 9, 25},
    {"s32i3p04", 32, 32, 26}, {"s32n3p04", 32, 32, 26}, {"s33i3p04", 33, 33, 27},
    {"s33n3p04", 33, 33, 27}, {"s34i3p04", 34, 34, 28}, {"s34n3p04", 34, 34, 28},
    {"s35i3p04", 35, 35, 29}, {"s35n3p04", 35, 35, 29}, {"s36i3p04", 36, 36, 30},
    {"s36n3p04", 36, 36, 30}, {"s37i3p04", 37, 37, 31}, {"s37n3p04", 37, 37, 31},
    {"s38i3p04", 38, 38, 32}, {"s38n3p04", 38, 38, 32}, {"s39i3p04", 39, 39, 33},
    {"s39n3p04", 39, 39, 33}, {"s40i3p04", 40, 40, 34}, {"s40n3p04", 40, 40, 34},
    {"tbbn0g04", 32, 32, 0},  {"tbbn2c16", 32, 32, 35}, {"tbbn3p08", 32, 32, 36},
    {"tbgn2c16", 32, 32, 35}, {"tbgn3p08", 32, 32, 36}, {"tbrn2c08", 32, 32, 0},
    {"tbwn0g16", 32, 32, 0},  {"tbwn3p08", 32, 32, 36}, {"tbyn3p08", 32, 32, 36},
    {"tm3n3p02", 32, 32, 0},  {"tp0n0g08", 32, 32, 0},  {"tp0n2c08", 32, 32, 0},
    {"tp0n3p08", 32, 32, 0},  {"tp1n3p08", 32, 32, 36}, {"xc1n0g08", 0, 0, 0},
    {"xc9n2c08", 0, 0, 0},    {"xcrn0g04", 0, 0, 0},    {"xcsn0g01", 0, 0, 0},
    {"xd0n2c08", 0, 0, 0},    {"xd3n2c08", 0, 0, 0},    {"xd9n2c08", 0, 0, 0},
    {"xdtn0g01", 0, 0, 0},    {"xhdn0g08", 0, 0, 0},    {"xlfn0g04", 0, 0, 0},
    {"xs1n0g01", 0, 0, 0},    {"xs2n0g01", 0, 0, 0},    {"xs4n0g01", 0, 0, 0},
    {"xs7n0g01", 0, 0, 0},    {"z00n2c08", 32, 32, 37}, {"z03n2c08", 32, 32, 37},
    {"z06n2c08", 32, 32, 37}, {"z09n2c08", 32, 32, 37},
};

// 묶음 번호의 상한이다(표의 최댓값보다 크다).
constexpr uint32 MaxGroups = 64;
// 경로 버퍼 크기다.
constexpr usize PathCapacity = 4096;
// PngSuite 파일 하나의 byte 상한이다(가장 큰 PngSuite.png가 수 KiB).
constexpr uint64 MaxSuiteFileBytes = uint64{1} << 20;
constexpr StringView SuiteDirectory = "/Engine/Source/Developer/ImageCodec/Tests/Fixtures/PngSuite";

bool IsSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

// 실행 파일 위치에서 위로 올라가며 저장소의 PngSuite 디렉터리를 찾아 out에 쓴다.
bool FindSuite(FixedStringBuilder& out) noexcept {
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
        if (!out.TryAppendFormat("{}{}", StringView(executable, length), SuiteDirectory)) {
            return false;
        }

        platform::FileInfo info;
        if (files.Stat(out.View().Data(), out.View().ByteCount(), info) &&
            info.type == platform::FileType::Directory) {
            return true;
        }
    }

    return false;
}

} // namespace

TEST(PngDecode_PngSuite) {
    SystemAllocator allocator;
    {
        char directoryText[PathCapacity];
        FixedStringBuilder directory{Span<char>(directoryText)};
        REQUIRE(FindSuite(directory));
        ImageDecodeLimits limits;
        // 묶음마다 첫 파일의 결과(형식·크기·픽셀)다.
        bool seen[MaxGroups] = {};
        TextureSourceFormat formats[MaxGroups] = {};
        uint32 widths[MaxGroups] = {};
        uint32 heights[MaxGroups] = {};
        Sha256Digest pixels[MaxGroups] = {};
        uint32 failures = 0;
        uint32 decoded = 0;
        for (const SuiteFile& file : SuiteFiles) {
            REQUIRE(file.group < MaxGroups);
            char pathText[PathCapacity];
            FixedStringBuilder path{Span<char>(pathText)};
            REQUIRE(path.TryAppendFormat("{}/{}.png", directory.View(), file.name));
            Array<uint8> bytes(allocator);
            bool existed = false;
            REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), path.View(), MaxSuiteFileBytes,
                                  bytes, existed));
            CHECK(existed);
            LiveCountingAllocator live(allocator);
            {
                ImageDecodeResult result(live);
                const bool ok = static_cast<bool>(
                    DecodeImage(live, bytes.View(), ImageFileFormat::Png, limits, result));
                if (file.width == 0) {
                    ++failures;
                    CHECK(!ok && result.failure.code == AssetDiagnosticCode::InvalidSource &&
                          result.failure.error == ErrorCode::CorruptData);
                } else {
                    ++decoded;
                    const DecodedImage& image = result.image;
                    CHECK(ok && ConsistentImage(image, limits) && image.width == file.width &&
                          image.height == file.height);
                    if (ok && file.group != 0) {
                        const Sha256Digest digest = Sha256::Hash(image.pixels.View());
                        if (!seen[file.group]) {
                            seen[file.group] = true;
                            formats[file.group] = image.format;
                            widths[file.group] = image.width;
                            heights[file.group] = image.height;
                            pixels[file.group] = digest;
                        } else {
                            CHECK(image.format == formats[file.group] &&
                                  image.width == widths[file.group] &&
                                  image.height == heights[file.group] &&
                                  digest == pixels[file.group]);
                        }
                    }
                }
            }

            CHECK(live.Live() == 0);
        }

        CHECK(failures == 14 && decoded == 162);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace png_suite_tests_file
