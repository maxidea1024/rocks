#include "BlockGoldenImages.h"

#include <Core/Containers/Array.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Cpu.h>
#include <Platform/Memory.h>
#include <Platform/DynamicLibrary.h>
#include <Launch/EntryPoint.h>
#include <TextureTools/TextureBlockEncode.h>

#include <ispc_texcomp.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Rocks BC6H·BC7 인코더와 UE 5.8 ISPC Texture Compressor DLL의 품질
// 대조다(Docs/Design/AssetSystem 12.17.6 10, TC3c). 시험 그림 셋(32 × 32·128 × 128)을 같은
// 입력·같은 프로필로 양쪽에서 압축하고 같은 기준 디코더(Texture TextureBlockDecode)로 풀어 PSNR을
// 비교한다. 판정은 묶음 전체 Rocks ≥ UE − 0.05 dB, 그림마다 Rocks ≥ UE − 0.3 dB다(UE DLL은
// fast-math·다중 타깃이라 byte는 다르다).
//
// 사용: TextureCodecCompare --ue-dll <UE
// 클론>/Engine/Binaries/ThirdParty/Intel/ISPCTexComp/Win64-Release/ispc_texcomp.dll
using namespace rocks;

namespace {

namespace golden = test::block_golden;

constexpr uint32 Channels = 4;
constexpr uint32 Bptc = 16;
constexpr uint32 ColorChannels = 3;
constexpr double PerfectPsnr = 99.0;
constexpr double LdrPeak = 255.0;
// 판정 여유다(12.17.6 7).
constexpr double AggregateMargin = 0.05;
constexpr double ImageMargin = 0.3;
// 거의 무손실인 그림(PSNR 60 dB대 — 몇 texel이 1 단계 어긋나는 차이)은 dB 차이가 과장되므로, LDR
// 그림은 평균 제곱 오차(8비트 단위²)가 UE보다 이만큼 이하로 크면 통과로 본다(12.17.6 10 결과 —
// fast-math 반올림 운의 차이).
constexpr double NearLosslessMseAllowance = 0.1;
// 압축 방식 셋: BC7 RGB 프로필(RGB 오차), BC7 알파 프로필(RGBA 오차), BC6H(log2 공간 RGB 오차).
constexpr uint32 Modes = 3;
constexpr uint32 ModeBc7 = 0;
constexpr uint32 ModeBc7Alpha = 1;
constexpr uint32 ModeBc6h = 2;
constexpr const char* ModeNames[Modes] = {"BC7", "BC7a", "BC6H"};
// 한쪽 오차가 0이어서 차이가 무한일 때 출력하는 값이다(판정은 부호로 한다).
constexpr double InfiniteDiff = 99.0;

using ProfileBc7 = void (*)(bc7_enc_settings*);
using ProfileBc6h = void (*)(bc6h_enc_settings*);
using CompressBc7 = void (*)(const rgba_surface*, uint8_t*, bc7_enc_settings*);
using CompressBc6h = void (*)(const rgba_surface*, uint8_t*, bc6h_enc_settings*);

struct UeEncoder {
    ProfileBc7 basic = nullptr;
    ProfileBc7 alpha = nullptr;
    ProfileBc6h bc6h = nullptr;
    CompressBc7 compressBc7 = nullptr;
    CompressBc6h compressBc6h = nullptr;
};

// 오차 제곱 합·값 수·봉우리다(묶음 전체 PSNR은 합을 이어 계산한다).
struct ErrorSum {
    double squared = 0.0;
    double count = 0.0;
    double peak = 0.0;

    void Add(const ErrorSum& other) noexcept {
        squared += other.squared;
        count += other.count;
        peak = other.peak > peak ? other.peak : peak;
    }

    double Psnr() const noexcept {
        if (squared == 0.0 || peak == 0.0) {
            return PerfectPsnr;
        }

        return 10.0 * log10(peak * peak * count / squared);
    }
};

// rocks − ue PSNR 차이(dB)다. 같은 그림이라 봉우리·값 수가 같으므로 오차 제곱 합의 비로 바로
// 계산한다(봉우리 0이나 99 dB 자름과 무관하다). 한쪽만 오차 0이면 ±InfiniteDiff, 둘 다 0이면 0이다.
double PsnrDiff(const ErrorSum& rocks, const ErrorSum& ue) noexcept {
    if (rocks.squared == 0.0 || ue.squared == 0.0) {
        return rocks.squared == ue.squared ? 0.0
                                           : (rocks.squared == 0.0 ? InfiniteDiff : -InfiniteDiff);
    }

    return 10.0 * log10((ue.squared / ue.count) / (rocks.squared / rocks.count));
}

bool OpenUe(const char* path, platform::DynamicLibrary& library,
            platform::DynamicLibraryLease& lease, UeEncoder& ue) noexcept {
    if (!library.Open(path)) {
        printf("cannot open %s\n", path);
        return false;
    }

    platform::DynamicSymbol symbols[] = {{"GetProfile_basic", nullptr},
                                         {"GetProfile_alpha_basic", nullptr},
                                         {"GetProfile_bc6h_basic", nullptr},
                                         {"CompressBlocksBC7", nullptr},
                                         {"CompressBlocksBC6H", nullptr}};
    if (!library.Find(symbols, sizeof(symbols) / sizeof(symbols[0]), lease)) {
        printf("the DLL does not export the BC6H/BC7 entry points\n");
        return false;
    }

    ue.basic = reinterpret_cast<ProfileBc7>(symbols[0].address);
    ue.alpha = reinterpret_cast<ProfileBc7>(symbols[1].address);
    ue.bc6h = reinterpret_cast<ProfileBc6h>(symbols[2].address);
    ue.compressBc7 = reinterpret_cast<CompressBc7>(symbols[3].address);
    ue.compressBc6h = reinterpret_cast<CompressBc6h>(symbols[4].address);
    return true;
}

// 시험 HdrFrom과 같은 HDR 그림이다(채널마다 v² × 배율, 알파 1.0 — BC6H 입력 정리 뒤와 같은 값).
void HdrFrom(const uint8* rgba, usize texels, uint8* out) noexcept {
    constexpr float Scale[ColorChannels] = {64.0f, 4.0f, 0.25f};
    for (usize i = 0; i < texels; ++i) {
        for (uint32 c = 0; c < Channels; ++c) {
            const float unit = static_cast<float>(rgba[i * Channels + c]) / 255.0f;
            const float value = c == 3 ? 1.0f : unit * unit * Scale[c];
            const uint16 bits = Float16::FromFloat(value).bits;
            out[i * TextureBlockHalfTexelBytes + c * 2] = static_cast<uint8>(bits & 0xFF);
            out[i * TextureBlockHalfTexelBytes + c * 2 + 1] = static_cast<uint8>(bits >> 8);
        }
    }
}

float HalfAt(const uint8* texels, usize index, uint32 channel) noexcept {
    const uint8* p = texels + index * TextureBlockHalfTexelBytes + channel * 2;
    return Float16{static_cast<uint16>(p[0] | (p[1] << 8))}.ToFloat();
}

// 블록 출력(sizeX × sizeY, 4의 배수)을 기준 디코더로 풀어 원본과 비교한다.
ErrorSum Measure(uint32 mode, const uint8* blocks, const uint8* rgba, const uint8* hdr,
                 uint32 sizeX, uint32 sizeY) noexcept {
    ErrorSum sum;
    const uint32 blocksX = sizeX / TextureBlockSize;
    for (uint32 by = 0; by < sizeY / TextureBlockSize; ++by) {
        for (uint32 bx = 0; bx < blocksX; ++bx) {
            const uint8* block = blocks + (usize{by} * blocksX + bx) * Bptc;
            uint8 decoded[TextureBlockRgbaBytes];
            uint16 halfs[TextureBlockTexels * Channels];
            if (mode == ModeBc6h) {
                DecodeTextureBlockHalf(block, halfs);
            } else {
                DecodeTextureBlock(TextureBlockFormat::Bc7, block, decoded);
            }

            for (uint32 t = 0; t < TextureBlockTexels; ++t) {
                const usize index = (usize{by} * TextureBlockSize + t / TextureBlockSize) * sizeX +
                                    bx * TextureBlockSize + t % TextureBlockSize;
                const uint32 channels = mode == ModeBc7Alpha ? Channels : ColorChannels;
                for (uint32 c = 0; c < channels; ++c) {
                    double original = 0.0;
                    double got = 0.0;
                    if (mode == ModeBc6h) {
                        original = log2(1.0 + HalfAt(hdr, index, c));
                        got = log2(1.0 + Float16{halfs[t * Channels + c]}.ToFloat());
                    } else {
                        original = rgba[index * Channels + c];
                        got = decoded[t * Channels + c];
                    }

                    sum.squared += (got - original) * (got - original);
                    sum.count += 1.0;
                    sum.peak =
                        mode == ModeBc6h ? (original > sum.peak ? original : sum.peak) : LdrPeak;
                }
            }
        }
    }

    return sum;
}

int Compare(const char* dllPath) noexcept {
    platform::DynamicLibrary library;
    platform::DynamicLibraryLease lease;
    UeEncoder ue;
    if (!OpenUe(dllPath, library, lease, ue)) {
        return 1;
    }

    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    printf("CPU: sse4.2 %d, avx %d, avx2 %d (the UE DLL dispatches to the widest of "
           "sse2/sse4/avx/avx2)\n",
           cpu.sse42 ? 1 : 0, cpu.avx ? 1 : 0, cpu.avx2 ? 1 : 0);
    printf("Rocks ISPC: %.*s\n", static_cast<int>(IspcBcEncoderIdentity().ByteCount()),
           IspcBcEncoderIdentity().Data());
    SystemAllocator allocator;
    bc7_enc_settings basic{};
    bc7_enc_settings alpha{};
    bc6h_enc_settings bc6h{};
    ue.basic(&basic);
    ue.alpha(&alpha);
    ue.bc6h(&bc6h);
    ErrorSum totalRocks[Modes];
    ErrorSum totalUe[Modes];
    bool pass = true;
    printf("%-22s", "image");
    for (const char* name : ModeNames) {
        printf(" %9s %9s %7s", name, "UE", "diff");
    }

    printf("\n");
    for (const golden::Case& one : golden::Cases) {
        // 측정 그림과 드문 경로 256 × 256(해시만인 그림 — 퇴화에 가까운 공분산)을 모두 본다. 4의
        // 배수가 아닌 가장자리 그림은 채움 규칙이 달라 뺀다(EngineTests가 본다).
        if (one.sizeX < 32) {
            continue;
        }

        const usize texels = usize{one.sizeX} * one.sizeY;
        Array<uint8> rgba(allocator);
        Array<uint8> hdr(allocator);
        Array<uint8> rocksBlocks(allocator);
        Array<uint8> ueBlocks(allocator);
        if (!rgba.TryResize(texels * Channels) ||
            !hdr.TryResize(texels * TextureBlockHalfTexelBytes) || !rocksBlocks.TryResize(texels) ||
            !ueBlocks.TryResize(texels)) {
            printf("out of memory\n");
            return 1;
        }

        golden::Generate(one, rgba.Data());
        HdrFrom(rgba.Data(), texels, hdr.Data());
        printf("%-22s", one.name);
        for (uint32 mode = ModeBc7; mode < Modes; ++mode) {
            const bool half = mode == ModeBc6h;
            TextureBlockImage image;
            image.rgba = half ? hdr.View() : rgba.View();
            image.sizeX = one.sizeX;
            image.sizeY = one.sizeY;
            TextureBlockOptions options;
            options.alphaProfile = mode == ModeBc7Alpha;
            if (!EncodeTextureBlocks(half ? TextureBlockFormat::Bc6h : TextureBlockFormat::Bc7,
                                     image, options, rocksBlocks.View())) {
                printf("\nRocks encoding failed\n");
                return 1;
            }

            const usize texelBytes = half ? TextureBlockHalfTexelBytes : Channels;
            rgba_surface surface{half ? hdr.Data() : rgba.Data(), static_cast<int32_t>(one.sizeX),
                                 static_cast<int32_t>(one.sizeY),
                                 static_cast<int32_t>(one.sizeX * texelBytes)};
            if (half) {
                ue.compressBc6h(&surface, ueBlocks.Data(), &bc6h);
            } else {
                ue.compressBc7(&surface, ueBlocks.Data(), mode == ModeBc7Alpha ? &alpha : &basic);
            }

            const ErrorSum rocks =
                Measure(mode, rocksBlocks.Data(), rgba.Data(), hdr.Data(), one.sizeX, one.sizeY);
            const ErrorSum unreal =
                Measure(mode, ueBlocks.Data(), rgba.Data(), hdr.Data(), one.sizeX, one.sizeY);
            totalRocks[mode].Add(rocks);
            totalUe[mode].Add(unreal);
            const double diff = PsnrDiff(rocks, unreal);
            const bool nearLossless =
                mode != ModeBc6h && rocks.squared / rocks.count - unreal.squared / unreal.count <=
                                        NearLosslessMseAllowance;
            pass = pass && (diff >= -ImageMargin || nearLossless);
            printf(" %9.3f %9.3f %+7.3f", rocks.Psnr(), unreal.Psnr(), diff);
        }

        printf("\n");
    }

    printf("%-22s", "aggregate");
    for (uint32 mode = ModeBc7; mode < Modes; ++mode) {
        const double diff = PsnrDiff(totalRocks[mode], totalUe[mode]);
        pass = pass && diff >= -AggregateMargin;
        printf(" %9.3f %9.3f %+7.3f", totalRocks[mode].Psnr(), totalUe[mode].Psnr(), diff);
    }

    printf("\n%s (aggregate >= UE - %.2f dB; every image >= UE - %.1f dB, or for BC7 a mean "
           "squared error at most %.2f above "
           "UE)\n",
           pass ? "PASS" : "FAIL", AggregateMargin, ImageMargin, NearLosslessMseAllowance);
    lease.Reset();
    (void)library.Close();
    return pass ? 0 : 1;
}

int RunMain(launch::ApplicationContext& context) noexcept {
    const Span<const StringView> arguments = context.Arguments();
    if (arguments.Size() != 2 || !platform::CStringEquals(arguments[0].Data(), "--ue-dll")) {
        printf("usage: TextureCodecCompare --ue-dll <absolute path of ispc_texcomp.dll>\n");
        return 1;
    }

    if (IspcBcEncoderIdentity().IsEmpty()) {
        printf("this build has no BC6H/BC7 encoder\n");
        return 1;
    }

    return Compare(arguments[1].Data());
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = "TextureCodecCompare", .kind = launch::ApplicationKind::Tool, .run = &RunMain};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
