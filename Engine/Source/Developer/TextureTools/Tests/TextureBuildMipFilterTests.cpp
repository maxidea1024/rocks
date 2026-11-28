#include "ProcessingGoldenImages.h"
#include "../Private/TextureBuildSteps.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Process.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureBuild.h>

#include <stdio.h>
#include <string.h>

// mip 필터 시험이다(Docs/Design/AssetSystem 12.18 TP1 — 대조표 14.1.127): 커널 표와 mip 사슬이 UE
// 원본 probe(Tools/TextureProcessingProbe) 기준 판 golden과 byte까지 같다. 경우 표는
// ProcessingGoldenImages.h에 있다(필터 Sharpen·Blur·SimpleAverage·Unfiltered, 새·옛 경로, 주소
// 방식, alpha coverage, PreserveBorder, Volume·Cube·CubeArray·2DArray, 최대 크기 단계 경로).
using namespace rocks;
namespace texture_build_mip_filter_tests_file {

namespace {

namespace pg = rocks::test::processing_golden;
using texture_build_detail::Level;
using texture_build_detail::build_kernel::BuildMipKernel;
using texture_build_detail::build_kernel::MipKernel;
using texture_build_detail::build_mips::GenerateMipChain;
using texture_build_detail::build_mips::MipChainLength;
using texture_build_detail::build_mips::MipChainParams;
using texture_build_detail::build_mips::MipChainParamsFor;

constexpr usize PathCapacity = 4096;
constexpr uint64 MaxGoldenBytes = uint64{16} << 20;
constexpr StringView KernelGoldenPath =
    "/Engine/Source/Developer/TextureTools/Tests/Fixtures/TextureProcessingGolden/KernelGolden.bin";
constexpr StringView MipChainGoldenPath = "/Engine/Source/Developer/TextureTools/Tests/Fixtures/"
                                          "TextureProcessingGolden/MipChainGolden.bin";
constexpr uint32 KernelGoldenVersion = 1;
constexpr uint32 MipChainGoldenVersion = 1;

bool IsSeparator(char value) noexcept {
    return value == '/' || value == '\\';
}

// 실행 파일 위치에서 위로 올라가며 저장소 뿌리의 golden을 찾는다.
bool FindGolden(StringView relative, FixedStringBuilder& out) noexcept {
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
        if (!out.TryAppendFormat("{}{}", StringView(executable, length), relative)) {
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

bool LoadGolden(StringView relative, Array<uint8>& file) noexcept {
    char pathText[PathCapacity];
    FixedStringBuilder path{Span<char>(pathText)};
    bool existed = false;
    return FindGolden(relative, path) &&
           ReadWholeFile(platform::PhysicalFileSystem(), path.View(), MaxGoldenBytes, file,
                         existed) &&
           existed;
}

struct Reader {
    Span<const uint8> bytes;
    usize at = 0;
    bool ok = true;

    bool Has(usize count) noexcept {
        ok = ok && at <= bytes.Size() && bytes.Size() - at >= count;
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

    float F32() noexcept {
        return platform::BitCast<float>(U32());
    }

    Span<const uint8> Take(usize count) noexcept {
        if (!Has(count)) {
            return {};
        }

        const Span<const uint8> taken = bytes.Slice(at, count);
        at += count;
        return taken;
    }
};

bool SameBits(float a, float b) noexcept {
    return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
}

// 경우 표 한 줄을 Rocks 빌드 설정으로 옮긴다(UE probe의 SettingsFor와 같은 입력).
TextureBuildSettings SettingsFor(const pg::MipCase& one) noexcept {
    TextureBuildSettings settings;
    switch (one.dimension) {
    case pg::Volume:
        settings.dimension = TextureDimension::Volume;
        break;
    case pg::Cube:
        settings.dimension = TextureDimension::TextureCube;
        break;
    case pg::CubeArray:
        settings.dimension = TextureDimension::TextureCubeArray;
        break;
    case pg::Array:
        settings.dimension = TextureDimension::Texture2DArray;
        break;
    default:
        settings.dimension = TextureDimension::Texture2D;
        break;
    }

    settings.sizeX = one.sizeX;
    settings.sizeY = one.sizeY;
    settings.numSlices = one.slices;
    settings.mipGenSettings = static_cast<TextureMipGenSettings>(one.mipGen);
    settings.compression = static_cast<TextureCompressionSettings>(one.compression);
    settings.useNewMipFilter = one.useNewMipFilter;
    settings.preserveBorder = one.preserveBorder;
    settings.addressX = static_cast<TextureAddress>(one.addressX);
    settings.addressY = static_cast<TextureAddress>(one.addressY);
    const double thresholds[4] = {one.thresholdMilli[0] / 1000.0, one.thresholdMilli[1] / 1000.0,
                                  one.thresholdMilli[2] / 1000.0, one.thresholdMilli[3] / 1000.0};
    settings.alphaCoverage = thresholds[0] != 0.0 || thresholds[1] != 0.0 || thresholds[2] != 0.0 ||
                             thresholds[3] != 0.0;
    if (settings.alphaCoverage) {
        settings.alphaCoverageThresholds =
            Vec4{static_cast<float>(thresholds[0]), static_cast<float>(thresholds[1]),
                 static_cast<float>(thresholds[2]), static_cast<float>(thresholds[3])};
    }

    return settings;
}

// UE probe BuildChain과 같은 순서다: 최대 크기 단계 경로(GenerateMipChain(…, 1) 반복) → mip 0 →
// 나머지 사슬.
Result<void> BuildChain(Allocator& allocator, const pg::MipCase& one,
                        Array<Level>& chain) noexcept {
    const TextureBuildSettings settings = SettingsFor(one);
    const MipChainParams params = MipChainParamsFor(settings);
    const bool volume = settings.dimension == TextureDimension::Volume;
    Level top(allocator);
    if (auto made =
            top.Allocate(one.sizeX, one.sizeY, volume ? one.slices : 1, volume ? 1 : one.slices);
        !made) {
        return made;
    }

    pg::Generate(one, top.texels.Data());
    if (one.maxSize != 0) {
        while (top.width > one.maxSize || top.height > one.maxSize) {
            Array<Level> next(allocator);
            if (auto reserved = next.TryReserve(1); !reserved) {
                return reserved;
            }

            if (auto made = GenerateMipChain(allocator, params, top, 1, next); !made) {
                return made;
            }

            top = rocks::Move(next[0]);
        }
    }

    const uint32 planes = top.layers * top.depth;
    const uint32 count =
        one.mipGen == pg::NoMipmaps ? 1 : MipChainLength(params, top.width, top.height, planes) + 1;
    chain.Clear();
    if (auto reserved = chain.TryReserve(count); !reserved) {
        return reserved;
    }

    (void)chain.TryPushBack(rocks::Move(top));
    return GenerateMipChain(allocator, params, chain[0], count - 1, chain);
}

} // namespace

// 커널 표(12.18 TP1 1): Rocks BuildMipKernel이 UE FImageKernel2D의 19개 커널(SimpleAverage·Blur1–5·
// Sharpen0–10·합성 6·7탭 Gauss)과 1D·2D 가중치 bit까지 같다.
TEST(TextureBuild_MipKernelsMatchUe) {
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(LoadGolden(KernelGoldenPath, file));
    Reader reader{file.View()};
    const Span<const uint8> magic = reader.Take(4);
    REQUIRE(reader.ok && memcmp(magic.Data(), "RTPK", 4) == 0);
    REQUIRE(reader.U32() == KernelGoldenVersion);
    const uint32 count = reader.U32();
    REQUIRE(reader.ok && count == 19);
    for (uint32 entry = 0; entry < count; ++entry) {
        const uint32 size = reader.U32();
        const float sharpen = reader.F32();
        REQUIRE(reader.ok && size >= 2 && size <= 12);
        const MipKernel kernel = BuildMipKernel(size, sharpen);
        CHECK(kernel.size == size);
        for (uint32 x = 0; x < size; ++x) {
            CHECK(SameBits(kernel.weights1D[x], reader.F32()));
        }

        for (uint32 index = 0; index < size * size; ++index) {
            CHECK(SameBits(kernel.weights2D[index], reader.F32()));
        }
    }

    CHECK(reader.ok && reader.at == file.Size());
}

// mip 사슬(12.18 TP1 2·3): 경우 표 49개의 출력 mip 수·크기·FNV-1a 해시(작은 경우는 float 전부)가 UE
// 원본 GenerateMipChain(probe 기준 판)과 같다.
TEST(TextureBuild_MipChainsMatchUe) {
    SystemAllocator allocator;
    Array<uint8> file(allocator);
    REQUIRE(LoadGolden(MipChainGoldenPath, file));
    Reader reader{file.View()};
    const Span<const uint8> magic = reader.Take(4);
    REQUIRE(reader.ok && memcmp(magic.Data(), "RTPM", 4) == 0);
    REQUIRE(reader.U32() == MipChainGoldenVersion);
    REQUIRE(reader.U32() == pg::MipCaseCount);
    for (const pg::MipCase& one : pg::MipCases) {
        const usize nameBytes = reader.U32();
        const Span<const uint8> name = reader.Take(nameBytes);
        REQUIRE(reader.ok && nameBytes == strlen(one.name) &&
                memcmp(name.Data(), one.name, nameBytes) == 0);
        Array<Level> chain(allocator);
        REQUIRE(BuildChain(allocator, one, chain));
        const uint32 mips = reader.U32();
        CHECK(chain.Size() == mips);
        uint64 floats = 0;
        for (uint32 mip = 0; mip < mips; ++mip) {
            const uint32 sizeX = reader.U32();
            const uint32 sizeY = reader.U32();
            const uint32 slices = reader.U32();
            const uint64 hash = reader.U64();
            floats += uint64{sizeX} * sizeY * slices * 4;
            if (mip >= chain.Size()) {
                continue;
            }

            const Level& level = chain[mip];
            CHECK(level.width == sizeX && level.height == sizeY &&
                  level.layers * level.depth == slices);
            const auto* bytes = reinterpret_cast<const unsigned char*>(level.texels.Data());
            const uint64 actual = pg::Fnv1a64(bytes, level.texels.Size() * sizeof(float));
            if (actual != hash) {
                fprintf(stderr, "%s mip %u differs from the UE golden\n", one.name, mip);
                CHECK(false);
            }
        }

        if (floats <= pg::FullDataFloats) {
            for (uint32 mip = 0; mip < mips && mip < chain.Size(); ++mip) {
                const Level& level = chain[mip];
                for (usize index = 0; index < level.texels.Size(); ++index) {
                    if (!SameBits(level.texels[index], reader.F32())) {
                        fprintf(stderr, "%s mip %u float %zu differs\n", one.name, mip,
                                static_cast<size_t>(index));
                        CHECK(false);
                        break;
                    }
                }
            }
        }
    }

    CHECK(reader.ok && reader.at == file.Size());
}

// 할당 실패(12.18 TP1 7): 평균 사슬 임시 그림·행 버퍼·Volume 두 번째 slice·출력 mip의 모든 할당
// 지점에서 OutOfMemory이고, 다 지나면 golden과 같은 사슬이다.
TEST(TextureBuild_MipChainAllocationFailures) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    const pg::MipCase* targets[] = {&pg::MipCases[0], nullptr, nullptr};
    for (const pg::MipCase& one : pg::MipCases) {
        if (strcmp(one.name, "sh5-volume-16x16x4-n") == 0) {
            targets[1] = &one;
        } else if (strcmp(one.name, "sh5-cov-32-n") == 0) {
            targets[2] = &one;
        }
    }

    for (const pg::MipCase* one : targets) {
        REQUIRE(one != nullptr);
        Array<Level> expected(system);
        REQUIRE(BuildChain(system, *one, expected));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            Array<Level> chain(failing);
            auto built = BuildChain(failing, *one, chain);
            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!hit) {
                REQUIRE(built);
                break;
            }

            CHECK(!built && built.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures > 3);
        Array<Level> chain(system);
        REQUIRE(BuildChain(system, *one, chain));
        REQUIRE(chain.Size() == expected.Size());
        for (usize mip = 0; mip < chain.Size(); ++mip) {
            CHECK(chain[mip].texels.Size() == expected[mip].texels.Size() &&
                  memcmp(chain[mip].texels.Data(), expected[mip].texels.Data(),
                         chain[mip].texels.Size() * sizeof(float)) == 0);
        }
    }
}

// BuildTexture 경로(12.18 TP1 2·3·8): RGBA32F 원본을 HDR_F32(비압축 RGBA32Float — 양자화 없음)로
// 빌드하면 쿠킹 mip이 golden과 같은 사슬이다(설정 → 최대 크기 단계 경로 → 사슬 → 출력이 함수 시험과
// 같은 결과).
TEST(TextureBuild_MipFilterThroughBuildTexture) {
    SystemAllocator allocator;
    const char* names[] = {"sh5-32-n",      "blur3-32-o",    "sh5-cov-32-n",
                           "step-sh5-64-o", "sh5-cube-16-o", "step-blur3-volume-32x32x8-n"};
    for (const char* name : names) {
        const pg::MipCase* found = nullptr;
        for (const pg::MipCase& one : pg::MipCases) {
            found = strcmp(one.name, name) == 0 ? &one : found;
        }

        REQUIRE(found != nullptr);
        const pg::MipCase& one = *found;
        Array<Level> expected(allocator);
        REQUIRE(BuildChain(allocator, one, expected));
        TextureBuildSettings settings = SettingsFor(one);
        settings.format = TextureSourceFormat::RGBA32F;
        settings.compression = TextureCompressionSettings::HDR_F32;
        settings.srgb = false;
        settings.maxTextureSize = static_cast<int32>(one.maxSize);
        Array<float> texels(allocator);
        REQUIRE(texels.TryResize(usize{one.sizeX} * one.sizeY * one.slices * 4));
        pg::Generate(one, texels.Data());
        const Span<const uint8> pixels(reinterpret_cast<const uint8*>(texels.Data()),
                                       texels.Size() * sizeof(float));
        TextureBuildResult result(allocator);
        REQUIRE(BuildTexture(allocator, settings, pixels, result));
        CHECK(result.pixelFormat == graphics::PixelFormat::RGBA32Float);
        REQUIRE(result.mips.Size() == expected.Size());
        for (usize mip = 0; mip < expected.Size(); ++mip) {
            const usize bytes = expected[mip].texels.Size() * sizeof(float);
            const bool same =
                result.mips[mip].bytes.Size() == bytes &&
                memcmp(result.mips[mip].bytes.Data(), expected[mip].texels.Data(), bytes) == 0;
            if (!same) {
                fprintf(stderr, "%s cooked mip %zu differs\n", name, static_cast<size_t>(mip));
            }

            CHECK(same);
        }
    }
}

} // namespace texture_build_mip_filter_tests_file
