#include "TextureBlockBptc.h"

#include <Base/Compiler.h>
#include <Platform/Cpu.h>
#include <Platform/FloatingPointEnvironment.h>

// ISPC 커널이 있는 대상(데스크톱)은 ThirdParty IspcTextureCompressor가 ROCKS_ISPC_IDENTITY를
// 정의한다. 그 정의가 없으면(Android) BC6H·BC7은 Unsupported다(플랫폼 매크로 대신 커널 유무로
// 나눈다).
#if defined(ROCKS_ISPC_IDENTITY)
#include <ispc_texcomp.h>
#endif

// BC6H·BC7 인코더다(Docs/Design/AssetSystem 12.17.6 — UE 5.8 TextureFormatIntelISPCTexComp와 같은
// 처리). ISPC Texture Compressor 커널(ThirdParty IspcTextureCompressor, 고정 ISPC로 결정적 빌드)을
// 블록 행 strip마다 부른다. strip 버퍼는 스택에 두고, 복사하면서 가장자리 복제(UE
// PadImageToBlockSize)와 BC6H 입력 정리(UE SanitizeFloat16AndSetAlphaOpaqueForBC6H)를 한다. 블록이
// 서로 독립이므로 strip 경계는 출력에 영향이 없다.
namespace rocks::texture_block_detail {

#if defined(ROCKS_ISPC_IDENTITY)

namespace {

// 유니티 빌드에서 다른 번역 단위의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace ispc_encode {

// strip 하나의 블록 수(블록 행 하나 안)다. BC6H strip 버퍼는 4행 × 256 texel × 8 byte = 8 KiB다.
constexpr uint32 StripBlocks = 64;
constexpr uint32 StripTexels = StripBlocks * TextureBlockSize;
constexpr uint32 StripBytes = StripTexels * TextureBlockSize * TextureBlockHalfTexelBytes;
// BC6H·BC7 블록 byte 수다(TextureBlockBytes와 같다).
constexpr uint32 BptcBlockBytes = 16;
constexpr uint32 HalfBytes = 2;
constexpr uint32 ColorChannels = 3;
// half bit 값이다: 가장 큰 유한값, +Inf, 1.0.
constexpr uint16 HalfMaxFinite = 0x7BFF;
constexpr uint16 HalfInfinity = 0x7C00;
constexpr uint16 HalfOne = 0x3C00;

// UE FFloat16::GetClampedNonNegativeAndFinite: 음이 아닌 유한값은 그대로, +Inf는 가장 큰 유한값,
// NaN· 음수(−0 포함)는 0이다.
uint16 SanitizeHalf(uint16 value) noexcept {
    if (value < HalfInfinity) {
        return value;
    }

    return value == HalfInfinity ? HalfMaxFinite : uint16{0};
}

uint16 LoadLe16(const uint8* bytes) noexcept {
    return static_cast<uint16>(bytes[0] | (bytes[1] << 8));
}

void StoreLe16(uint8* bytes, uint16 value) noexcept {
    bytes[0] = static_cast<uint8>(value & 0xFFu);
    bytes[1] = static_cast<uint8>(value >> 8);
}

uint32 Smaller(uint32 a, uint32 b) noexcept {
    return a < b ? a : b;
}

struct Job {
    TextureBlockFormat format;
    const uint8* base;
    uint32 sizeX;
    uint32 sizeY;
    uint32 numSlices;
    uint64 rowPitch;
    uint64 slicePitch;
    TextureBlockOptions options;
    uint8* out;
};

// 결정적 부동소수 환경 안에서 돈다(RunWithDeterministicFloatingPoint — 커널은 호출자의 FTZ·DAZ를
// 따른다).
void Run(void* context) noexcept {
    Job& job = *static_cast<Job*>(context);
    const bool bc6h = job.format == TextureBlockFormat::Bc6h;
    // UE와 같은 고정 프로필이다(CompressionQuality를 보지 않는다 — TD13). BC7은 알파가 있으면 alpha
    // 프로필이다.
    bc7_enc_settings bc7{};
    bc6h_enc_settings bc6hSettings{};
    if (bc6h) {
        GetProfile_bc6h_basic(&bc6hSettings);
    } else if (job.options.alphaProfile) {
        GetProfile_alpha_basic(&bc7);
    } else {
        GetProfile_basic(&bc7);
    }

    const uint32 texelBytes = TextureBlockSourceTexelBytes(job.format);
    // uint64로 센다(크기 상한 근처에서 + 3이 넘치지 않게 — TextureBlockOutputBytes와 같은 식).
    const uint32 blocksX =
        static_cast<uint32>((uint64{job.sizeX} + TextureBlockSize - 1) / TextureBlockSize);
    const uint32 blocksY =
        static_cast<uint32>((uint64{job.sizeY} + TextureBlockSize - 1) / TextureBlockSize);
    alignas(16) uint8 strip[StripBytes];
    uint8* cursor = job.out;
    for (uint32 slice = 0; slice < job.numSlices; ++slice) {
        const uint8* base = job.base + job.slicePitch * slice;
        for (uint32 by = 0; by < blocksY; ++by) {
            for (uint32 bx = 0; bx < blocksX; bx += StripBlocks) {
                const uint32 blocks = Smaller(StripBlocks, blocksX - bx);
                const uint32 width = blocks * TextureBlockSize;
                for (uint32 row = 0; row < TextureBlockSize; ++row) {
                    // 블록 배수가 아닌 크기는 마지막 열·행을 복제해 채운다(UE PadImageToBlockSize).
                    const uint32 y = Smaller(by * TextureBlockSize + row, job.sizeY - 1);
                    const uint8* line = base + job.rowPitch * y;
                    for (uint32 column = 0; column < width; ++column) {
                        const uint32 x = Smaller(bx * TextureBlockSize + column, job.sizeX - 1);
                        const uint8* source = line + uint64{x} * texelBytes;
                        uint8* target = strip + (uint64{row} * width + column) * texelBytes;
                        if (bc6h) {
                            for (uint32 channel = 0; channel < ColorChannels; ++channel) {
                                StoreLe16(target + channel * HalfBytes,
                                          SanitizeHalf(LoadLe16(source + channel * HalfBytes)));
                            }

                            StoreLe16(target + ColorChannels * HalfBytes, HalfOne);
                        } else {
                            for (uint32 k = 0; k < texelBytes; ++k) {
                                target[k] = source[k];
                            }
                        }
                    }
                }

                rgba_surface surface{};
                surface.ptr = strip;
                surface.width = static_cast<int32_t>(width);
                surface.height = static_cast<int32_t>(TextureBlockSize);
                surface.stride = static_cast<int32_t>(width * texelBytes);
                if (bc6h) {
                    CompressBlocksBC6H(&surface, cursor, &bc6hSettings);
                } else {
                    CompressBlocksBC7(&surface, cursor, &bc7);
                }

                cursor += uint64{blocks} * BptcBlockBytes;
            }
        }
    }
}

} // namespace ispc_encode

} // namespace

#endif

Result<void> EncodeIspcBlocks(TextureBlockFormat format, const uint8* base, uint32 sizeX,
                              uint32 sizeY, uint32 numSlices, uint64 rowPitch, uint64 slicePitch,
                              const TextureBlockOptions& options, Span<uint8> out) noexcept {
#if !defined(ROCKS_ISPC_IDENTITY)
    (void)format;
    (void)base;
    (void)sizeX;
    (void)sizeY;
    (void)numSlices;
    (void)rowPitch;
    (void)slicePitch;
    (void)options;
    (void)out;
    return Result<void>::Failure(ErrorCode::Unsupported);
#else
#if ROCKS_ARCH_X64
    // x64 빌드는 ISPC sse4-i32x4 타깃 하나다(BuildTargets 3.12절 BD47). SSE4.2가 없으면 잘못된
    // 명령으로 죽기 전에 거부한다.
    if (!platform::QueryCpuFeatures().sse42) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
#endif

    ispc_encode::Job job{format,   base,       sizeX,   sizeY,     numSlices,
                         rowPitch, slicePitch, options, out.Data()};
    platform::RunWithDeterministicFloatingPoint(&ispc_encode::Run, &job);
    return Result<void>::Success();
#endif
}

} // namespace rocks::texture_block_detail
