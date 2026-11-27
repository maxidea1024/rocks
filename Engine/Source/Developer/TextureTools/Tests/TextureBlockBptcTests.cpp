#include "BlockGoldenImages.h"

#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureBlockEncode.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// BC6H·BC7 인코더(ISPC Texture Compressor 감싸기)와 기준 디코더 시험이다(Docs/Design/AssetSystem
// 12.17.6 9). UE DLL과의 품질 대조·아키텍처 간 결정성은 TC3c 도구가 본다. 여기서는 감싸기
// 규칙(strip 경계·가장자리 복제·BC6H 정리·프로필·부동소수 환경), 기준 디코더의 알려진 블록, 왕복
// 품질 하한, 출력 해시 golden을 본다. BC6H·BC7 인코더가 없는 대상(Android — IspcBcEncoderIdentity가
// 빈 값)에서는 Unsupported만 확인하고 나머지는 건너뛴다.
using namespace rocks;
namespace texture_block_bptc_tests_file {

namespace {

namespace golden = test::block_golden;

constexpr uint32 Channels = 4;
constexpr uint32 Bptc = 16;
constexpr uint16 HalfOne = 0x3C00;
// PSNR이 무한(오차 0)일 때 쓰는 값이다.
constexpr double PerfectPsnr = 99.0;
// 왕복 PSNR 하한이다(디코더·감싸기 오류를 잡는 값 — 노이즈 그림은 BC7도 18 dB대다, 2026-11-27
// 측정).
constexpr double MinimumPsnr = 15.0;
constexpr double MinimumHdrPsnr = 18.0;

// 블록 bit를 낮은 bit부터 쓴다(알려진 블록을 손으로 만든다).
struct BitWriter {
    uint8 bytes[Bptc] = {};
    uint32 position = 0;

    void Put(uint32 value, uint32 count) noexcept {
        for (uint32 i = 0; i < count; ++i) {
            if (((value >> i) & 1u) != 0) {
                bytes[position >> 3] =
                    static_cast<uint8>(bytes[position >> 3] | (1u << (position & 7)));
            }

            ++position;
        }
    }
};

bool Encode(Allocator& allocator, TextureBlockFormat format, const uint8* texels, uint32 x,
            uint32 y, bool alphaProfile, Array<uint8>& out, uint32 slices = 1, usize rowPitch = 0,
            usize slicePitch = 0) noexcept {
    (void)allocator;
    auto bytes = TextureBlockOutputBytes(format, x, y, slices);
    if (!bytes || !out.TryResize(static_cast<usize>(bytes.Value()))) {
        return false;
    }

    const usize texelBytes = TextureBlockSourceTexelBytes(format);
    const usize pitch = rowPitch == 0 ? usize{x} * texelBytes : rowPitch;
    const usize plane = slicePitch == 0 ? pitch * y : slicePitch;
    TextureBlockImage image;
    image.rgba =
        Span<const uint8>(texels, plane * (slices - 1) + pitch * (y - 1) + usize{x} * texelBytes);
    image.sizeX = x;
    image.sizeY = y;
    image.numSlices = slices;
    image.rowPitch = rowPitch;
    image.slicePitch = slicePitch;
    TextureBlockOptions options;
    options.alphaProfile = alphaProfile;
    return static_cast<bool>(EncodeTextureBlocks(format, image, options, out.View()));
}

bool GenerateRgba(const golden::Case& one, Array<uint8>& out) noexcept {
    if (!out.TryResize(usize{one.sizeX} * one.sizeY * Channels)) {
        return false;
    }

    golden::Generate(one, out.Data());
    return true;
}

// RGBA8 그림을 HDR RGBA16F로 만든다(채널마다 v² × 배율 — 0…64를 넘는 넓은 범위, 결정적).
bool HdrFrom(const Array<uint8>& rgba, Array<uint8>& out) noexcept {
    const usize texels = rgba.Size() / Channels;
    if (!out.TryResize(texels * TextureBlockHalfTexelBytes)) {
        return false;
    }

    constexpr float Scale[3] = {64.0f, 4.0f, 0.25f};
    for (usize i = 0; i < texels; ++i) {
        for (uint32 c = 0; c < Channels; ++c) {
            const float unit = static_cast<float>(rgba[i * Channels + c]) / 255.0f;
            const float value = c == 3 ? 1.0f : unit * unit * Scale[c];
            const uint16 bits = Float16::FromFloat(value).bits;
            out[i * TextureBlockHalfTexelBytes + c * 2] = static_cast<uint8>(bits & 0xFF);
            out[i * TextureBlockHalfTexelBytes + c * 2 + 1] = static_cast<uint8>(bits >> 8);
        }
    }

    return true;
}

uint16 HalfAt(const uint8* texels, usize index, uint32 channel) noexcept {
    const uint8* p = texels + index * TextureBlockHalfTexelBytes + channel * 2;
    return static_cast<uint16>(p[0] | (p[1] << 8));
}

double Psnr(double squared, double count, double peak) noexcept {
    if (squared == 0.0) {
        return PerfectPsnr;
    }

    return 10.0 * log10(peak * peak * count / squared);
}

// RGBA8 그림을 블록 형식으로 압축해 기준 디코더로 풀고 PSNR을 잰다(channels = 3이면 RGB, 4면 RGBA).
double Psnr8(Allocator& allocator, TextureBlockFormat format, const Array<uint8>& rgba, uint32 x,
             uint32 y, bool alphaProfile, uint32 channels) noexcept {
    Array<uint8> blocks(allocator);
    if (!Encode(allocator, format, rgba.Data(), x, y, alphaProfile, blocks)) {
        return -1.0;
    }

    const uint32 blocksX = (x + 3) / 4;
    double squared = 0.0;
    double count = 0.0;
    for (uint32 by = 0; by * 4 < y; ++by) {
        for (uint32 bx = 0; bx < blocksX; ++bx) {
            uint8 decoded[TextureBlockRgbaBytes];
            DecodeTextureBlock(
                format, blocks.Data() + (usize{by} * blocksX + bx) * TextureBlockBytes(format),
                decoded);
            for (uint32 t = 0; t < TextureBlockTexels; ++t) {
                const uint32 px = bx * 4 + t % 4;
                const uint32 py = by * 4 + t / 4;
                if (px >= x || py >= y) {
                    continue;
                }

                for (uint32 c = 0; c < channels; ++c) {
                    const double d = static_cast<double>(decoded[t * Channels + c]) -
                                     static_cast<double>(rgba[(usize{py} * x + px) * Channels + c]);
                    squared += d * d;
                    count += 1.0;
                }
            }
        }
    }

    return Psnr(squared, count, 255.0);
}

// BC6H 왕복 PSNR이다(log2(1 + x) 공간 — 12.17.6 7, 봉우리는 원본 최댓값).
double PsnrHdr(Allocator& allocator, const Array<uint8>& halfs, uint32 x, uint32 y) noexcept {
    Array<uint8> blocks(allocator);
    if (!Encode(allocator, TextureBlockFormat::Bc6h, halfs.Data(), x, y, false, blocks)) {
        return -1.0;
    }

    const uint32 blocksX = (x + 3) / 4;
    double squared = 0.0;
    double count = 0.0;
    double peak = 0.0;
    for (uint32 by = 0; by * 4 < y; ++by) {
        for (uint32 bx = 0; bx < blocksX; ++bx) {
            uint16 decoded[TextureBlockTexels * 4];
            DecodeTextureBlockHalf(blocks.Data() + (usize{by} * blocksX + bx) * Bptc, decoded);
            for (uint32 t = 0; t < TextureBlockTexels; ++t) {
                const uint32 px = bx * 4 + t % 4;
                const uint32 py = by * 4 + t / 4;
                if (px >= x || py >= y) {
                    continue;
                }

                for (uint32 c = 0; c < 3; ++c) {
                    const double original =
                        log2(1.0 + Float16{HalfAt(halfs.Data(), usize{py} * x + px, c)}.ToFloat());
                    const double got = log2(1.0 + Float16{decoded[t * Channels + c]}.ToFloat());
                    peak = original > peak ? original : peak;
                    squared += (got - original) * (got - original);
                    count += 1.0;
                }
            }
        }
    }

    return Psnr(squared, count, peak);
}

// 생성기 그림 가운데 품질·해시 시험이 쓰는 것(32 × 32와 128 × 128)이다.
bool Measured(const golden::Case& one) noexcept {
    return !one.hashOnly && one.sizeX >= 32;
}

// BC6H·BC7 인코더가 있는 대상인지다(ISPC 커널 — Android에는 없다).
bool HasIspcEncoder() noexcept {
    return !IspcBcEncoderIdentity().IsEmpty();
}

bool EncoderAvailable() noexcept {
    uint8 texels[4 * 4 * Channels] = {};
    uint8 out[Bptc];
    TextureBlockImage image;
    image.rgba = Span<const uint8>(texels, sizeof(texels));
    image.sizeX = 4;
    image.sizeY = 4;
    return static_cast<bool>(
        EncodeTextureBlocks(TextureBlockFormat::Bc7, image, Span<uint8>(out, Bptc)));
}

} // namespace

// 크기·형식 질의와 감싸기 신원이다. Android는 BC6H·BC7 인코딩이 Unsupported다.
TEST(TextureBlockBptc_ShapesAndAvailability) {
    CHECK(TextureBlockBytes(TextureBlockFormat::Bc6h) == 16 &&
          TextureBlockBytes(TextureBlockFormat::Bc7) == 16);
    CHECK(TextureBlockSourceTexelBytes(TextureBlockFormat::Bc6h) == 8 &&
          TextureBlockSourceTexelBytes(TextureBlockFormat::Bc7) == 4 &&
          TextureBlockSourceTexelBytes(TextureBlockFormat::Bc1) == 4);
    if (!HasIspcEncoder()) {
        CHECK(!EncoderAvailable());
    } else {
        CHECK(EncoderAvailable());
        const StringView identity = IspcBcEncoderIdentity();
        CHECK(identity.StartsWith("ispc ") && identity.Contains("--opt=disable-fma") &&
              !identity.Contains("fast-math"));
    }
    // 블록 하나 함수는 BC6H·BC7을 받지 않는다(0으로 채운다).
    uint8 texels[TextureBlockRgbaBytes];
    memset(texels, 0x7F, sizeof(texels));
    uint8 out[Bptc];
    memset(out, 0xCD, sizeof(out));
    EncodeTextureBlock(TextureBlockFormat::Bc7, texels, out);
    bool zero = true;
    for (const uint8 value : out) {
        zero = zero && value == 0;
    }

    CHECK(zero);
}

// 기준 디코더의 손으로 만든 블록이다(D3D11 BC7 mode 6·예약 모드, BC6H mode 11·예약 모드).
TEST(TextureBlockBptc_DecoderKnownBlocks) {
    // BC7 mode 6: RGBA 7777 + 끝점마다 p-bit, 4-bit index(texel 0은 3 bit).
    BitWriter mode6;
    mode6.Put(1u << 6, 7);
    const uint32 endpoints[4][2] = {{0x7F, 0x00}, {0x00, 0x7F}, {0x40, 0x40}, {0x7F, 0x7F}};
    for (const auto& channel : endpoints) {
        mode6.Put(channel[0], 7);
        mode6.Put(channel[1], 7);
    }

    mode6.Put(1, 1);
    mode6.Put(0, 1);
    mode6.Put(0, 3);
    mode6.Put(15, 4);
    for (uint32 t = 2; t < TextureBlockTexels; ++t) {
        mode6.Put(8, 4);
    }

    CHECK(mode6.position == 128);
    uint8 texels[TextureBlockRgbaBytes];
    DecodeTextureBlock(TextureBlockFormat::Bc7, mode6.bytes, texels);
    // 끝점(p-bit 1·0): (255, 1, 129, 255)와 (0, 254, 128, 254). index 8 가중치 34 → ((64 − 34)·a +
    // 34·b + 32) >> 6.
    CHECK(texels[0] == 255 && texels[1] == 1 && texels[2] == 129 && texels[3] == 255);
    CHECK(texels[4] == 0 && texels[5] == 254 && texels[6] == 128 && texels[7] == 254);
    CHECK(texels[8] == 120 && texels[9] == 135 && texels[10] == 128 && texels[11] == 254);
    uint8 reserved[Bptc] = {};
    DecodeTextureBlock(TextureBlockFormat::Bc7, reserved, texels);
    bool black = true;
    for (const uint8 value : texels) {
        black = black && value == 0;
    }

    CHECK(black);

    // BC7 mode 4: 1 subset, RGB 555 + A 6, rotation 1(R↔A), index 선택 1(색은 3-bit 둘째 index,
    // 알파는 2-bit 첫 index). 끝점 (255, 0, 132, 0)·(0, 0, 132, 255).
    BitWriter mode4;
    mode4.Put(1u << 4, 5);
    mode4.Put(1, 2);
    mode4.Put(1, 1);
    const uint32 mode4Colors[3][2] = {{31, 0}, {0, 0}, {16, 16}};
    for (const auto& channel : mode4Colors) {
        mode4.Put(channel[0], 5);
        mode4.Put(channel[1], 5);
    }

    mode4.Put(0, 6);
    mode4.Put(63, 6);
    // 첫 index(2 bit, texel 0은 1 bit): texel 1 = 3, 그 밖 1. 둘째 index(3 bit, texel 0은 2 bit):
    // texel 1 = 7, 그 밖 4.
    mode4.Put(0, 1);
    mode4.Put(3, 2);
    for (uint32 t = 2; t < TextureBlockTexels; ++t) {
        mode4.Put(1, 2);
    }

    mode4.Put(0, 2);
    mode4.Put(7, 3);
    for (uint32 t = 2; t < TextureBlockTexels; ++t) {
        mode4.Put(4, 3);
    }

    CHECK(mode4.position == 128);
    DecodeTextureBlock(TextureBlockFormat::Bc7, mode4.bytes, texels);
    // rotation 뒤: texel 0 = (0, 0, 132, 255), texel 1 = (255, 0, 132, 0), 그 밖 색 w37·알파 w21 =
    // (84, 0, 132, 108).
    CHECK(texels[0] == 0 && texels[1] == 0 && texels[2] == 132 && texels[3] == 255);
    CHECK(texels[4] == 255 && texels[5] == 0 && texels[6] == 132 && texels[7] == 0);
    CHECK(texels[8] == 84 && texels[9] == 0 && texels[10] == 132 && texels[11] == 108);

    // BC7 mode 1: 2 subset(모양 0 — texel 15가 subset 1의 anchor), RGB 666 + subset마다 공유 p-bit.
    BitWriter mode1;
    mode1.Put(1u << 1, 2);
    mode1.Put(0, 6);
    const uint32 mode1Colors[3][4] = {{63, 0, 0, 32}, {0, 63, 0, 32}, {0, 0, 63, 32}};
    for (const auto& channel : mode1Colors) {
        for (const uint32 value : channel) {
            mode1.Put(value, 6);
        }
    }

    mode1.Put(1, 1);
    mode1.Put(0, 1);
    for (uint32 t = 0; t < TextureBlockTexels; ++t) {
        const uint32 bits = t == 0 || t == 15 ? 2 : 3;
        const uint32 index = t == 1 ? 4 : t == 2 ? 7 : t == 15 ? 3 : 0;
        mode1.Put(index, bits);
    }

    CHECK(mode1.position == 128);
    DecodeTextureBlock(TextureBlockFormat::Bc7, mode1.bytes, texels);
    // subset 0 끝점 (255, 2, 2)·(2, 255, 2), subset 1 끝점 (0, 0, 253)·(129, 129, 129).
    CHECK(texels[0] == 255 && texels[1] == 2 && texels[2] == 2 && texels[3] == 255);
    CHECK(texels[4] == 109 && texels[5] == 148 && texels[6] == 2);
    CHECK(texels[8] == 129 && texels[9] == 129 && texels[10] == 129);
    CHECK(texels[60] == 54 && texels[61] == 54 && texels[62] == 201);

    // BC6H mode 11(0x03): 1 subset, 끝점 10 bit 그대로(변환 없음), 4-bit index.
    BitWriter mode11;
    mode11.Put(0x03, 5);
    mode11.Put(1023, 10);
    mode11.Put(0, 10);
    mode11.Put(0, 10);
    mode11.Put(0, 10);
    mode11.Put(512, 10);
    mode11.Put(0, 10);
    mode11.Put(0, 3);
    for (uint32 t = 1; t < TextureBlockTexels; ++t) {
        mode11.Put(15, 4);
    }

    CHECK(mode11.position == 128);
    uint16 halfs[TextureBlockTexels * 4];
    DecodeTextureBlockHalf(mode11.bytes, halfs);
    // R 1023 → 0xFFFF → (65535 · 31) >> 6 = 0x7BFF(65504). G 512 → ((512 << 16) + 0x8000) >> 10 =
    // 32800 → 0x3E0F.
    CHECK(halfs[0] == 0x7BFF && halfs[1] == 0 && halfs[2] == 0 && halfs[3] == HalfOne);
    CHECK(halfs[4] == 0 && halfs[5] == 0x3E0F && halfs[6] == 0 && halfs[7] == HalfOne);
    // BC6H mode 12(0x07): 1 subset 변환 — 끝점 0은 11 bit, 끝점 1은 9 bit 부호 있는 차이(R −100, G
    // +200, B +1은 11 bit에서 wrap).
    BitWriter mode12;
    mode12.Put(0x07, 5);
    mode12.Put(1500 & 1023, 10);
    mode12.Put(0, 10);
    mode12.Put(2047 & 1023, 10);
    mode12.Put(512 - 100, 9);
    mode12.Put(1500 >> 10, 1);
    mode12.Put(200, 9);
    mode12.Put(0, 1);
    mode12.Put(1, 9);
    mode12.Put(1, 1);
    mode12.Put(0, 3);
    for (uint32 t = 1; t < TextureBlockTexels; ++t) {
        mode12.Put(15, 4);
    }

    CHECK(mode12.position == 128);
    DecodeTextureBlockHalf(mode12.bytes, halfs);
    // 끝점 0 (1500, 0, 2047) → (0x5AD9, 0, 0x7BFF), 끝점 1 (1400, 200, 0) → (0x54CB, 0x0C23, 0).
    CHECK(halfs[0] == 0x5AD9 && halfs[1] == 0 && halfs[2] == 0x7BFF && halfs[3] == HalfOne);
    CHECK(halfs[4] == 0x54CB && halfs[5] == 0x0C23 && halfs[6] == 0 && halfs[7] == HalfOne);
    // 예약 모드 0x13은 불투명 검정이다.
    BitWriter reservedHdr;
    reservedHdr.Put(0x13, 5);
    DecodeTextureBlockHalf(reservedHdr.bytes, halfs);
    CHECK(halfs[0] == 0 && halfs[1] == 0 && halfs[2] == 0 && halfs[3] == HalfOne);
    // BC6H는 RGBA8 디코더로 풀지 않는다(0).
    DecodeTextureBlock(TextureBlockFormat::Bc6h, mode11.bytes, texels);
    CHECK(texels[0] == 0 && texels[3] == 0);
}

// 왕복 품질: BC7(RGB 프로필, 불투명 그림)은 BC1 이상이고, 알파 프로필은 RGBA에서 BC3 이상이다(모든
// 그림 — 2026-11-27 측정 BC7 18–61 dB, BC6H log2 공간 19–85 dB). 절대 하한은 디코더 오류를 잡는
// 값이다. 측정값을 출력한다(TC5 표의 바탕).
TEST(TextureBlockBptc_RoundTripQuality) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    for (const golden::Case& one : golden::Cases) {
        if (!Measured(one)) {
            continue;
        }

        Array<uint8> rgba(allocator);
        REQUIRE(GenerateRgba(one, rgba));
        Array<uint8> opaque(allocator);
        REQUIRE(opaque.TryResize(rgba.Size()));
        memcpy(opaque.Data(), rgba.Data(), rgba.Size());
        for (usize i = 3; i < opaque.Size(); i += Channels) {
            opaque[i] = 255;
        }

        const double bc1 =
            Psnr8(allocator, TextureBlockFormat::Bc1, opaque, one.sizeX, one.sizeY, false, 3);
        const double bc7 =
            Psnr8(allocator, TextureBlockFormat::Bc7, opaque, one.sizeX, one.sizeY, false, 3);
        const double bc3 =
            Psnr8(allocator, TextureBlockFormat::Bc3, rgba, one.sizeX, one.sizeY, false, 4);
        const double bc7a =
            Psnr8(allocator, TextureBlockFormat::Bc7, rgba, one.sizeX, one.sizeY, true, 4);
        Array<uint8> hdr(allocator);
        REQUIRE(HdrFrom(rgba, hdr));
        const double bc6h = PsnrHdr(allocator, hdr, one.sizeX, one.sizeY);
        printf("  %-22s BC1 %6.2f  BC7 %6.2f  BC3 %6.2f  BC7a %6.2f  BC6H %6.2f dB\n", one.name,
               bc1, bc7, bc3, bc7a, bc6h);
        CHECK(bc7 >= bc1 && bc7 >= MinimumPsnr);
        CHECK(bc7a >= bc3 && bc7a >= MinimumPsnr);
        CHECK(bc6h >= MinimumHdrPsnr);
    }
}

// strip 경계 무관: 블록 행이 strip 여럿(64블록마다)을 넘는 그림의 결과 = 블록마다 4 × 4 그림으로
// 따로 압축한 결과(블록은 서로 독립이다 — 12.17.6 5).
TEST(TextureBlockBptc_StripIndependent) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    constexpr uint32 Width = 520;
    constexpr uint32 Height = 8;
    golden::Case wide{"strip", golden::WhiteNoise, Width, Height, 91};
    Array<uint8> rgba(allocator);
    REQUIRE(rgba.TryResize(usize{Width} * Height * Channels));
    golden::Generate(wide, rgba.Data());
    Array<uint8> hdr(allocator);
    REQUIRE(HdrFrom(rgba, hdr));
    for (const TextureBlockFormat format : {TextureBlockFormat::Bc7, TextureBlockFormat::Bc6h}) {
        const Array<uint8>& source = format == TextureBlockFormat::Bc6h ? hdr : rgba;
        const usize texelBytes = TextureBlockSourceTexelBytes(format);
        Array<uint8> whole(allocator);
        REQUIRE(Encode(allocator, format, source.Data(), Width, Height, true, whole));
        bool same = true;
        for (uint32 by = 0; by < Height / 4; ++by) {
            for (uint32 bx = 0; bx < Width / 4; ++bx) {
                Array<uint8> one(allocator);
                // 블록 하나를 같은 그림 안에서 row pitch로 잘라 압축한다.
                REQUIRE(Encode(allocator, format,
                               source.Data() + (usize{by} * 4 * Width + usize{bx} * 4) * texelBytes,
                               4, 4, true, one, 1, usize{Width} * texelBytes));
                same =
                    same && memcmp(one.Data(), whole.Data() + (usize{by} * (Width / 4) + bx) * Bptc,
                                   Bptc) == 0;
            }
        }

        CHECK(same);
    }
}

// 가장자리: 4의 배수가 아닌 크기(블록보다 작은 mip과 12 → 6 같은 중간 mip)는 마지막 열·행을 복제해
// 채운 그림과 byte가 같다(UE PadImageToBlockSize — 12.17.6 4). slice 둘과 넓은 row pitch도 본다.
TEST(TextureBlockBptc_EdgeReplication) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    const uint32 sizes[][2] = {{1, 1}, {3, 5}, {6, 10}, {5, 7}, {12, 6}, {2, 9}};
    for (const auto& size : sizes) {
        const uint32 x = size[0];
        const uint32 y = size[1];
        const uint32 px = (x + 3) / 4 * 4;
        const uint32 py = (y + 3) / 4 * 4;
        golden::Case noise{"edge", golden::WhiteNoise, x, y, 100 + x * 16 + y};
        Array<uint8> rgba(allocator);
        REQUIRE(rgba.TryResize(usize{x} * y * Channels * 2));
        golden::Generate(noise, rgba.Data());
        golden::Case other{"edge-2", golden::ValueNoise, x, y, 200 + x * 16 + y};
        golden::Generate(other, rgba.Data() + usize{x} * y * Channels);
        Array<uint8> hdr(allocator);
        REQUIRE(HdrFrom(rgba, hdr));
        for (const TextureBlockFormat format :
             {TextureBlockFormat::Bc7, TextureBlockFormat::Bc6h}) {
            const Array<uint8>& source = format == TextureBlockFormat::Bc6h ? hdr : rgba;
            const usize texelBytes = TextureBlockSourceTexelBytes(format);
            // 손으로 채운 그림(slice 둘)과, 행 끝에 여백 8 byte를 둔 원본 그림.
            Array<uint8> padded(allocator);
            REQUIRE(padded.TryResize(usize{px} * py * texelBytes * 2));
            const usize pitch = usize{x} * texelBytes + 8;
            Array<uint8> pitched(allocator);
            REQUIRE(pitched.TryResize(pitch * y * 2));
            memset(pitched.Data(), 0xEE, pitched.Size());
            for (uint32 slice = 0; slice < 2; ++slice) {
                for (uint32 row = 0; row < py; ++row) {
                    for (uint32 column = 0; column < px; ++column) {
                        const uint32 sx = column < x ? column : x - 1;
                        const uint32 sy = row < y ? row : y - 1;
                        memcpy(padded.Data() +
                                   ((usize{slice} * py + row) * px + column) * texelBytes,
                               source.Data() + ((usize{slice} * y + sy) * x + sx) * texelBytes,
                               texelBytes);
                    }
                }

                for (uint32 row = 0; row < y; ++row) {
                    memcpy(pitched.Data() + (usize{slice} * y + row) * pitch,
                           source.Data() + (usize{slice} * y + row) * x * texelBytes,
                           usize{x} * texelBytes);
                }
            }

            Array<uint8> want(allocator);
            REQUIRE(Encode(allocator, format, padded.Data(), px, py, true, want, 2));
            Array<uint8> got(allocator);
            REQUIRE(
                Encode(allocator, format, pitched.Data(), x, y, true, got, 2, pitch, pitch * y));
            CHECK(got.Size() == want.Size() && memcmp(got.Data(), want.Data(), want.Size()) == 0);
        }
    }
}

// BC6H 입력 정리: NaN·음수(−0 포함)·−Inf는 0, +Inf는 65504, 알파는 무시(1.0)다 — 정리한 입력과
// byte가 같다(UE SanitizeFloat16AndSetAlphaOpaqueForBC6H).
TEST(TextureBlockBptc_Bc6hSanitize) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    const uint16 raw[] = {0x7E00, 0xBC00, 0x8000, 0x7C00, 0xFC00, 0x7BFF, 0x3C00, 0x0001,
                          0x7C01, 0xFFFF, 0x4900, 0x0400, 0x3555, 0x8001, 0x5640, 0x2E66};
    const uint16 clean[] = {0, 0, 0,      0x7BFF, 0,      0x7BFF, 0x3C00, 0x0001,
                            0, 0, 0x4900, 0x0400, 0x3555, 0,      0x5640, 0x2E66};
    uint8 dirty[TextureBlockTexels * TextureBlockHalfTexelBytes];
    uint8 sanitized[TextureBlockTexels * TextureBlockHalfTexelBytes];
    for (uint32 t = 0; t < TextureBlockTexels; ++t) {
        for (uint32 c = 0; c < Channels; ++c) {
            const uint32 k = (t + c * 5) % TextureBlockTexels;
            const uint16 in = c == 3 ? static_cast<uint16>(0x1234 + t) : raw[k];
            const uint16 out = c == 3 ? HalfOne : clean[k];
            dirty[t * 8 + c * 2] = static_cast<uint8>(in & 0xFF);
            dirty[t * 8 + c * 2 + 1] = static_cast<uint8>(in >> 8);
            sanitized[t * 8 + c * 2] = static_cast<uint8>(out & 0xFF);
            sanitized[t * 8 + c * 2 + 1] = static_cast<uint8>(out >> 8);
        }
    }

    Array<uint8> a(allocator);
    Array<uint8> b(allocator);
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc6h, dirty, 4, 4, false, a));
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc6h, sanitized, 4, 4, false, b));
    CHECK(memcmp(a.Data(), b.Data(), Bptc) == 0);
}

// 프로필: 알파 그림에서 RGB 프로필은 알파를 255로 쓰고 알파 프로필은 알파를 지킨다. 두 결과는
// 다르고 각각 결정적이다.
TEST(TextureBlockBptc_Bc7Profiles) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    const golden::Case* alpha = nullptr;
    for (const golden::Case& one : golden::Cases) {
        if (StringView(one.name) == "alpha-edge") {
            alpha = &one;
        }
    }

    REQUIRE(alpha != nullptr);
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(*alpha, rgba));
    Array<uint8> rgb(allocator);
    Array<uint8> withAlpha(allocator);
    Array<uint8> again(allocator);
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), alpha->sizeX, alpha->sizeY,
                   false, rgb));
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), alpha->sizeX, alpha->sizeY,
                   true, withAlpha));
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), alpha->sizeX, alpha->sizeY,
                   true, again));
    CHECK(memcmp(rgb.Data(), withAlpha.Data(), rgb.Size()) != 0);
    CHECK(memcmp(withAlpha.Data(), again.Data(), again.Size()) == 0);
    bool opaque = true;
    for (usize block = 0; block < rgb.Size() / Bptc; ++block) {
        uint8 texels[TextureBlockRgbaBytes];
        DecodeTextureBlock(TextureBlockFormat::Bc7, rgb.Data() + block * Bptc, texels);
        for (uint32 t = 0; t < TextureBlockTexels; ++t) {
            opaque = opaque && texels[t * Channels + 3] == 255;
        }
    }

    CHECK(opaque);
    CHECK(Psnr8(allocator, TextureBlockFormat::Bc7, rgba, alpha->sizeX, alpha->sizeY, true, 4) >=
          30.0);
}

// 부동소수 환경: 호출자가 FTZ·DAZ·0 쪽 반올림을 켜 두어도 같은 byte다(감싸기가 결정적 환경에서
// 압축한다 — 12.17.6 3).
TEST(TextureBlockBptc_FloatingPointEnvironment) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    const golden::Case& one = golden::Cases[2];
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    Array<uint8> hdr(allocator);
    REQUIRE(HdrFrom(rgba, hdr));
    // 비정규 half(아주 작은 값)도 섞는다.
    for (usize i = 0; i < hdr.Size(); i += 24) {
        hdr[i] = 0x03;
        hdr[i + 1] = 0x00;
    }

    Array<uint8> plain7(allocator);
    Array<uint8> plain6(allocator);
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), one.sizeX, one.sizeY, true,
                   plain7));
    REQUIRE(Encode(allocator, TextureBlockFormat::Bc6h, hdr.Data(), one.sizeX, one.sizeY, false,
                   plain6));
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit() |
                                                platform::detail::RoundTowardZeroBits());
    Array<uint8> disturbed7(allocator);
    Array<uint8> disturbed6(allocator);
    const bool encoded7 = Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), one.sizeX,
                                 one.sizeY, true, disturbed7);
    const bool encoded6 = Encode(allocator, TextureBlockFormat::Bc6h, hdr.Data(), one.sizeX,
                                 one.sizeY, false, disturbed6);
    const uint64 after = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original);
    REQUIRE(encoded7 && encoded6);
    CHECK(memcmp(plain7.Data(), disturbed7.Data(), plain7.Size()) == 0);
    CHECK(memcmp(plain6.Data(), disturbed6.Data(), plain6.Size()) == 0);
    // 감싸기는 호출자 설정을 되돌린다.
    CHECK((after & ~platform::detail::StatusFlagBits()) ==
          ((original | platform::detail::FlushToZeroBit() |
            platform::detail::DenormalsAreZeroBit() | platform::detail::RoundTowardZeroBits()) &
           ~platform::detail::StatusFlagBits()));
}

// 출력 해시 golden: 시험 그림 셋(32 × 32와 128 × 128)의 BC7(RGB·알파 프로필)·BC6H 출력 FNV-1a 64를
// 이어 해시한다. 커널·고정 ISPC·인자·감싸기 규칙이 바뀌면 바뀐다(그때는 결정성 도구로 확인한 뒤
// 갱신한다). x64(sse4)와 Arm64(neon)는 다른 값일 수 있다 — Arm64 값은 Mac 확인 때 기록한다.
TEST(TextureBlockBptc_OutputHashGolden) {
    if (!HasIspcEncoder()) {
        return;
    }

    SystemAllocator allocator;
    unsigned long long hash = golden::FnvOffset;
    for (const golden::Case& one : golden::Cases) {
        if (!Measured(one)) {
            continue;
        }

        Array<uint8> rgba(allocator);
        REQUIRE(GenerateRgba(one, rgba));
        Array<uint8> hdr(allocator);
        REQUIRE(HdrFrom(rgba, hdr));
        Array<uint8> out(allocator);
        REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), one.sizeX, one.sizeY, false,
                       out));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
        REQUIRE(Encode(allocator, TextureBlockFormat::Bc7, rgba.Data(), one.sizeX, one.sizeY, true,
                       out));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
        REQUIRE(Encode(allocator, TextureBlockFormat::Bc6h, hdr.Data(), one.sizeX, one.sizeY, false,
                       out));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
    }

    printf("  BC6H/BC7 output hash: %016llx\n", hash);
#if ROCKS_ARCH_X64
    CHECK(hash == 0xd7ca6ffb39737d3aull);
#endif
}

// BC6H 입력 pitch 검증은 RGBA16F texel byte로 한다.
TEST(TextureBlockBptc_ArgumentErrors) {
    if (!HasIspcEncoder()) {
        return;
    }

    uint8 texels[4 * 4 * TextureBlockHalfTexelBytes] = {};
    uint8 out[Bptc];
    TextureBlockImage image;
    image.rgba = Span<const uint8>(texels, sizeof(texels));
    image.sizeX = 4;
    image.sizeY = 4;
    image.rowPitch = 4 * 4;
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc6h, image, Span<uint8>(out, Bptc)).Error() ==
          ErrorCode::InvalidArgument);
    image.rowPitch = 0;
    image.rgba = Span<const uint8>(texels, sizeof(texels) - 1);
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc6h, image, Span<uint8>(out, Bptc)).Error() ==
          ErrorCode::InvalidArgument);
    image.rgba = Span<const uint8>(texels, sizeof(texels));
    CHECK(
        EncodeTextureBlocks(TextureBlockFormat::Bc6h, image, Span<uint8>(out, Bptc - 1)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(EncodeTextureBlocks(TextureBlockFormat::Bc6h, image, Span<uint8>(out, Bptc)));
}

} // namespace texture_block_bptc_tests_file
