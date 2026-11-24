#include "TextureImportInternal.h"

#include <Core/Assert.h>
#include <Base/NumericLimits.h>
#include <Platform/Memory.h>

// 텍스처 임포트의 이미지 처리 구현이다(Docs/Design/AssetSystem 12.12절). PNG infill은 UE
// TextureImportUtils.cpp TPNGDataFill과 같은 순서·같은 float 연산으로 해서 같은 화소를 낸다(곱은
// 따로 변수에 받아 FMA로 합쳐지지 않게 한다 — 빌드 때 UE 결과와 대조, 14.1.97절).
namespace rocks::texture_import {
namespace {

// sRGB 8비트 → 선형 float 표(UE FLinearColor::sRGBToLinearTable과 같은 float 값 — 공식
// c > 0.04045 ? ((c / 1.055) + 0.0521327)^2.4 : c / 12.92를 double로 계산해 float로 반올림했다).
constexpr float SrgbToLinear[256] = {
    0x0.0p+0f,
    0x1.3e45680000000p-12f,
    0x1.3e45680000000p-11f,
    0x1.dd681c0000000p-11f,
    0x1.3e45680000000p-10f,
    0x1.8dd6c20000000p-10f,
    0x1.dd681c0000000p-10f,
    0x1.167cba0000000p-9f,
    0x1.3e45680000000p-9f,
    0x1.660e140000000p-9f,
    0x1.8dd6c20000000p-9f,
    0x1.b6a31a0000000p-9f,
    0x1.e1e31c0000000p-9f,
    0x1.07c38c0000000p-8f,
    0x1.1fcc2c0000000p-8f,
    0x1.390ffa0000000p-8f,
    0x1.53936c0000000p-8f,
    0x1.6f5adc0000000p-8f,
    0x1.8c6a940000000p-8f,
    0x1.aac6c00000000p-8f,
    0x1.ca73820000000p-8f,
    0x1.eb74e00000000p-8f,
    0x1.06e76c0000000p-7f,
    0x1.18c2a60000000p-7f,
    0x1.2b4e0a0000000p-7f,
    0x1.3e8b7a0000000p-7f,
    0x1.527cd60000000p-7f,
    0x1.6723ee0000000p-7f,
    0x1.7c82920000000p-7f,
    0x1.929a880000000p-7f,
    0x1.a96d920000000p-7f,
    0x1.c0fd660000000p-7f,
    0x1.d94bbe0000000p-7f,
    0x1.f25a440000000p-7f,
    0x1.0615500000000p-6f,
    0x1.135f3e0000000p-6f,
    0x1.210bb80000000p-6f,
    0x1.2f1b8c0000000p-6f,
    0x1.3d8f840000000p-6f,
    0x1.4c68660000000p-6f,
    0x1.5ba6fa0000000p-6f,
    0x1.6b4c040000000p-6f,
    0x1.7b58420000000p-6f,
    0x1.8bcc740000000p-6f,
    0x1.9ca9580000000p-6f,
    0x1.adefaa0000000p-6f,
    0x1.bfa0200000000p-6f,
    0x1.d1bb740000000p-6f,
    0x1.e442580000000p-6f,
    0x1.f735840000000p-6f,
    0x1.054ad40000000p-5f,
    0x1.0f31ba0000000p-5f,
    0x1.194fcc0000000p-5f,
    0x1.23a55e0000000p-5f,
    0x1.2e32c80000000p-5f,
    0x1.38f8600000000p-5f,
    0x1.43f6780000000p-5f,
    0x1.4f2d620000000p-5f,
    0x1.5a9d760000000p-5f,
    0x1.6647000000000p-5f,
    0x1.722a560000000p-5f,
    0x1.7e47c80000000p-5f,
    0x1.8a9fa20000000p-5f,
    0x1.97323a0000000p-5f,
    0x1.a3ffd80000000p-5f,
    0x1.b108d00000000p-5f,
    0x1.be4d6c0000000p-5f,
    0x1.cbcdfa0000000p-5f,
    0x1.d98ac60000000p-5f,
    0x1.e7841c0000000p-5f,
    0x1.f5ba480000000p-5f,
    0x1.0216ca0000000p-4f,
    0x1.096f260000000p-4f,
    0x1.10e65c0000000p-4f,
    0x1.187c900000000p-4f,
    0x1.2031e80000000p-4f,
    0x1.2806860000000p-4f,
    0x1.2ffa900000000p-4f,
    0x1.380e2a0000000p-4f,
    0x1.4041740000000p-4f,
    0x1.4894940000000p-4f,
    0x1.5107ac0000000p-4f,
    0x1.599ade0000000p-4f,
    0x1.624e4e0000000p-4f,
    0x1.6b221e0000000p-4f,
    0x1.7416700000000p-4f,
    0x1.7d2b640000000p-4f,
    0x1.8661200000000p-4f,
    0x1.8fb7c00000000p-4f,
    0x1.992f680000000p-4f,
    0x1.a2c83a0000000p-4f,
    0x1.ac82560000000p-4f,
    0x1.b65ddc0000000p-4f,
    0x1.c05aec0000000p-4f,
    0x1.ca79a80000000p-4f,
    0x1.d4ba300000000p-4f,
    0x1.df1ca20000000p-4f,
    0x1.e9a1200000000p-4f,
    0x1.f447c80000000p-4f,
    0x1.ff10bc0000000p-4f,
    0x1.04fe0c0000000p-3f,
    0x1.0a84fe0000000p-3f,
    0x1.101d440000000p-3f,
    0x1.15c6ee0000000p-3f,
    0x1.1b82080000000p-3f,
    0x1.214ea60000000p-3f,
    0x1.272cd40000000p-3f,
    0x1.2d1ca20000000p-3f,
    0x1.331e1e0000000p-3f,
    0x1.3931580000000p-3f,
    0x1.3f56600000000p-3f,
    0x1.458d420000000p-3f,
    0x1.4bd60e0000000p-3f,
    0x1.5230d40000000p-3f,
    0x1.589da00000000p-3f,
    0x1.5f1c820000000p-3f,
    0x1.65ad8a0000000p-3f,
    0x1.6c50c20000000p-3f,
    0x1.73063e0000000p-3f,
    0x1.79ce060000000p-3f,
    0x1.80a82e0000000p-3f,
    0x1.8794c00000000p-3f,
    0x1.8e93ca0000000p-3f,
    0x1.95a55e0000000p-3f,
    0x1.9cc9860000000p-3f,
    0x1.a400520000000p-3f,
    0x1.ab49ce0000000p-3f,
    0x1.b2a60a0000000p-3f,
    0x1.ba15120000000p-3f,
    0x1.c196f40000000p-3f,
    0x1.c92bbe0000000p-3f,
    0x1.d0d37c0000000p-3f,
    0x1.d88e3c0000000p-3f,
    0x1.e05c0e0000000p-3f,
    0x1.e83cfc0000000p-3f,
    0x1.f031160000000p-3f,
    0x1.f838660000000p-3f,
    0x1.00297e0000000p-2f,
    0x1.0440720000000p-2f,
    0x1.0861160000000p-2f,
    0x1.0c8b700000000p-2f,
    0x1.10bf860000000p-2f,
    0x1.14fd600000000p-2f,
    0x1.1945020000000p-2f,
    0x1.1d96760000000p-2f,
    0x1.21f1be0000000p-2f,
    0x1.2656e40000000p-2f,
    0x1.2ac5ec0000000p-2f,
    0x1.2f3ede0000000p-2f,
    0x1.33c1c00000000p-2f,
    0x1.384e980000000p-2f,
    0x1.3ce56c0000000p-2f,
    0x1.4186420000000p-2f,
    0x1.4631200000000p-2f,
    0x1.4ae60e0000000p-2f,
    0x1.4fa5100000000p-2f,
    0x1.546e2c0000000p-2f,
    0x1.59416c0000000p-2f,
    0x1.5e1ed00000000p-2f,
    0x1.6306640000000p-2f,
    0x1.67f82a0000000p-2f,
    0x1.6cf4280000000p-2f,
    0x1.71fa680000000p-2f,
    0x1.770aec0000000p-2f,
    0x1.7c25ba0000000p-2f,
    0x1.814adc0000000p-2f,
    0x1.867a540000000p-2f,
    0x1.8bb4280000000p-2f,
    0x1.90f8600000000p-2f,
    0x1.9647000000000p-2f,
    0x1.9ba0100000000p-2f,
    0x1.a103940000000p-2f,
    0x1.a671920000000p-2f,
    0x1.abea100000000p-2f,
    0x1.b16d140000000p-2f,
    0x1.b6faa40000000p-2f,
    0x1.bc92c60000000p-2f,
    0x1.c2357e0000000p-2f,
    0x1.c7e2d20000000p-2f,
    0x1.cd9aca0000000p-2f,
    0x1.d35d6a0000000p-2f,
    0x1.d92ab60000000p-2f,
    0x1.df02b80000000p-2f,
    0x1.e4e5700000000p-2f,
    0x1.ead2e80000000p-2f,
    0x1.f0cb260000000p-2f,
    0x1.f6ce2c0000000p-2f,
    0x1.fcdc000000000p-2f,
    0x1.017a560000000p-1f,
    0x1.048c180000000p-1f,
    0x1.07a34a0000000p-1f,
    0x1.0abfee0000000p-1f,
    0x1.0de2080000000p-1f,
    0x1.11099a0000000p-1f,
    0x1.1436a80000000p-1f,
    0x1.1769320000000p-1f,
    0x1.1aa13c0000000p-1f,
    0x1.1ddeca0000000p-1f,
    0x1.2121de0000000p-1f,
    0x1.246a7a0000000p-1f,
    0x1.27b8a00000000p-1f,
    0x1.2b0c540000000p-1f,
    0x1.2e65980000000p-1f,
    0x1.31c46e0000000p-1f,
    0x1.3528dc0000000p-1f,
    0x1.3892e00000000p-1f,
    0x1.3c02800000000p-1f,
    0x1.3f77bc0000000p-1f,
    0x1.42f29a0000000p-1f,
    0x1.4673180000000p-1f,
    0x1.49f93e0000000p-1f,
    0x1.4d850a0000000p-1f,
    0x1.5116800000000p-1f,
    0x1.54ada40000000p-1f,
    0x1.584a780000000p-1f,
    0x1.5becfe0000000p-1f,
    0x1.5f95380000000p-1f,
    0x1.6343280000000p-1f,
    0x1.66f6d40000000p-1f,
    0x1.6ab03a0000000p-1f,
    0x1.6e6f600000000p-1f,
    0x1.7234480000000p-1f,
    0x1.75fef40000000p-1f,
    0x1.79cf640000000p-1f,
    0x1.7da59e0000000p-1f,
    0x1.8181a40000000p-1f,
    0x1.8563780000000p-1f,
    0x1.894b1c0000000p-1f,
    0x1.8d38920000000p-1f,
    0x1.912bde0000000p-1f,
    0x1.9525000000000p-1f,
    0x1.9923fe0000000p-1f,
    0x1.9d28d80000000p-1f,
    0x1.a133920000000p-1f,
    0x1.a5442c0000000p-1f,
    0x1.a95aac0000000p-1f,
    0x1.ad77100000000p-1f,
    0x1.b1995e0000000p-1f,
    0x1.b5c1980000000p-1f,
    0x1.b9efbe0000000p-1f,
    0x1.be23d40000000p-1f,
    0x1.c25dde0000000p-1f,
    0x1.c69ddc0000000p-1f,
    0x1.cae3d00000000p-1f,
    0x1.cf2fc00000000p-1f,
    0x1.d381aa0000000p-1f,
    0x1.d7d9940000000p-1f,
    0x1.dc377e0000000p-1f,
    0x1.e09b6a0000000p-1f,
    0x1.e5055c0000000p-1f,
    0x1.e975560000000p-1f,
    0x1.edeb5a0000000p-1f,
    0x1.f2676c0000000p-1f,
    0x1.f6e98c0000000p-1f,
    0x1.fb71bc0000000p-1f,
    0x1.0000000000000p+0f,
};

// 이웃 가중치 표 exp(-1.1 * sqrt(r1² + r2²))(UE TPNGDataFill NearNeighborWeights — 중심 0).
constexpr float InfillWeights[InfillRadius + 1][InfillRadius + 1] = {
    {0x0.0p+0f, 0x1.54dc280000000p-2f, 0x1.c5d9860000000p-4f, 0x1.2e25980000000p-5f,
     0x1.924dc80000000p-7f},
    {0x1.54dc280000000p-2f, 0x1.b03dc60000000p-3f, 0x1.5e0e2c0000000p-4f, 0x1.f980e40000000p-6f,
     0x1.5f5a620000000p-7f},
    {0x1.c5d9860000000p-4f, 0x1.5e0e2c0000000p-4f, 0x1.6ce8460000000p-5f, 0x1.366dbc0000000p-6f,
     0x1.deaac20000000p-8f},
    {0x1.2e25980000000p-5f, 0x1.f980e40000000p-6f, 0x1.366dbc0000000p-6f, 0x1.34100c0000000p-7f,
     0x1.0bd4a60000000p-8f},
    {0x1.924dc80000000p-7f, 0x1.5f5a620000000p-7f, 0x1.deaac20000000p-8f, 0x1.0bd4a60000000p-8f,
     0x1.0412ac0000000p-9f},
};

// stb_image_resize의 선형 float → sRGB 8비트 표다(UE Color.cpp stb_fp32_to_srgb8_tab4 — stb는
// public domain). 항목마다 상위 16비트 bias, 하위 16비트 scale이다.
constexpr uint32 LinearToSrgbTable[104] = {
    0x0073000d, 0x007a000d, 0x0080000d, 0x0087000d, 0x008d000d, 0x0094000d, 0x009a000d, 0x00a1000d,
    0x00a7001a, 0x00b4001a, 0x00c1001a, 0x00ce001a, 0x00da001a, 0x00e7001a, 0x00f4001a, 0x0101001a,
    0x010e0033, 0x01280033, 0x01410033, 0x015b0033, 0x01750033, 0x018f0033, 0x01a80033, 0x01c20033,
    0x01dc0067, 0x020f0067, 0x02430067, 0x02760067, 0x02aa0067, 0x02dd0067, 0x03110067, 0x03440067,
    0x037800ce, 0x03df00ce, 0x044600ce, 0x04ad00ce, 0x051400ce, 0x057b00c5, 0x05dd00bc, 0x063b00b5,
    0x06970158, 0x07420142, 0x07e30130, 0x087b0120, 0x090b0112, 0x09940106, 0x0a1700fc, 0x0a9500f2,
    0x0b0f01cb, 0x0bf401ae, 0x0ccb0195, 0x0d950180, 0x0e56016e, 0x0f0d015e, 0x0fbc0150, 0x10630143,
    0x11070264, 0x1238023e, 0x1357021d, 0x14660201, 0x156601e9, 0x165a01d3, 0x174401c0, 0x182401af,
    0x18fe0331, 0x1a9602fe, 0x1c1502d2, 0x1d7e02ad, 0x1ed4028d, 0x201a0270, 0x21520256, 0x227d0240,
    0x239f0443, 0x25c003fe, 0x27bf03c4, 0x29a10392, 0x2b6a0367, 0x2d1d0341, 0x2ebe031f, 0x304d0300,
    0x31d105b0, 0x34a80555, 0x37520507, 0x39d504c5, 0x3c37048b, 0x3e7c0458, 0x40a8042a, 0x42bd0401,
    0x44c20798, 0x488e071e, 0x4c1c06b6, 0x4f76065d, 0x52a50610, 0x55ac05cc, 0x5892058f, 0x5b590559,
    0x5e0c0a23, 0x631c0980, 0x67db08f6, 0x6c55087f, 0x70940818, 0x74a007bd, 0x787d076c, 0x7c330723,
};

// 표 변환의 범위 끝이다: 2^-13(→ 0)과 1 − eps(→ 255)의 float 비트, 표 index 비트 이동.
constexpr uint32 LinearToSrgbMinBits = (127u - 13u) << 23;
constexpr uint32 LinearToSrgbAlmostOneBits = 0x3f7fffffu;
constexpr uint32 LinearToSrgbIndexShift = 20;
constexpr uint32 LinearToSrgbBiasShift = 16;
constexpr uint32 LinearToSrgbBiasScale = 9;
constexpr uint32 LinearToSrgbScaleMask = 0xffffu;
constexpr uint32 LinearToSrgbMantissaShift = 12;
constexpr uint32 LinearToSrgbMantissaMask = 0xffu;
constexpr uint32 LinearToSrgbResultShift = 16;

// UE FLinearColor::Clamp01NansTo0다(NaN·음수 → 0, 1 초과 → 1).
float Clamp01NansTo0(float value) noexcept {
    return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
}

// 화소 형식 하나의 접근이다: 채널 수(마지막이 알파), 채널 형식, 최대값.
template <class TChannel, usize TChannels>
struct PixelLayout {
    using Channel = TChannel;
    static constexpr usize Channels = TChannels;
    static constexpr usize ColorChannels = TChannels - 1;
    static constexpr TChannel Max = NumericLimits<TChannel>::Max();

    // 채널 하나의 선형 float다(UE MakeLinearColor — 8비트 색은 sRGB 표, 8비트 알파는 a × (1/255),
    // 16비트는 모든 채널 v × (1/65535)).
    static float ToLinear(TChannel value, bool alpha) noexcept {
        if constexpr (sizeof(TChannel) == 1) {
            return alpha ? static_cast<float>(value) * (1.0f / 255.0f) : SrgbToLinear[value];
        } else {
            (void)alpha;
            return static_cast<float>(value) * (1.0f / 65535.0f);
        }
    }

    // 선형 float → 채널 값이다(UE MakeColorFromLinear — 8비트 색은 ToFColorSRGB, 16비트는
    // QuantizeUNormFloatTo16).
    static TChannel FromLinear(float value) noexcept {
        if constexpr (sizeof(TChannel) == 1) {
            return LinearToSrgb8(value);
        } else {
            return static_cast<TChannel>(0.5f + Clamp01NansTo0(value) * 65535.0f);
        }
    }
};

// UE TPNGDataFill를 화소 형식 하나로 한다(data는 width × height × Channels 채널).
template <class TLayout>
class TransparentFill {
public:
    using Channel = typename TLayout::Channel;
    static constexpr usize Channels = TLayout::Channels;
    static constexpr usize Alpha = TLayout::ColorChannels;

    TransparentFill(Channel* data, int64 width, int64 height) noexcept
        : data_(data), width_(width), height_(height) {
    }

    Result<void> Process(bool complexAlphaToo, Allocator& scratch) noexcept {
        // 알파 종류를 먼저 본다(UE ProcessData).
        bool whiteWithZeroAlpha = false;
        for (int64 index = 0; index < width_ * height_; ++index) {
            const Channel* pixel = data_ + index * Channels;
            if (pixel[Alpha] == TLayout::Max) {
                continue;
            }

            if (IsWhiteWithZeroAlpha(pixel)) {
                whiteWithZeroAlpha = true;
            } else if (!complexAlphaToo) {
                // 알파가 0·최대 밖이거나 알파 0이 흰색이 아니다 — 채우지 않는다.
                return Result<void>::Success();
            }
        }

        if (!whiteWithZeroAlpha) {
            return Result<void>::Success();
        }

        // 가까운 이웃으로 먼저 채우고(반지름 InfillRadius), 행 단위로 남은 곳을 채운다.
        if (auto filled = FillFromNearNeighbors(scratch); !filled) {
            return filled;
        }

        int64 zeroedTopRows = 0;
        int64 fillRow = -1;
        for (int64 y = 0; y < height_; ++y) {
            if (!ProcessRow(y)) {
                if (fillRow != -1) {
                    CopyRowColor(fillRow, y);
                } else {
                    zeroedTopRows = y + 1;
                }
            } else {
                fillRow = y;
            }
        }

        // 위쪽 빈 행은 처음 채워진 행에서 채운다(그림 전체가 비었으면 그대로).
        if (zeroedTopRows > 0 && zeroedTopRows < height_) {
            for (int64 y = 0; y < zeroedTopRows; ++y) {
                CopyRowColor(zeroedTopRows, y);
            }
        }

        return Result<void>::Success();
    }

private:
    Channel* Pixel(int64 x, int64 y) noexcept {
        return data_ + (y * width_ + x) * static_cast<int64>(Channels);
    }

    static bool IsWhiteWithZeroAlpha(const Channel* pixel) noexcept {
        for (usize channel = 0; channel < Alpha; ++channel) {
            if (pixel[channel] != TLayout::Max) {
                return false;
            }
        }

        return pixel[Alpha] == 0;
    }

    // 색은 from의 것, 알파는 0인 화소를 to에 쓴다(UE MakeColorWithZeroAlpha).
    static void SetColorWithZeroAlpha(Channel* to, const Channel* from) noexcept {
        for (usize channel = 0; channel < Alpha; ++channel) {
            to[channel] = from[channel];
        }

        to[Alpha] = 0;
    }

    // (centerX, centerY) 주변의 알파 있는 원본 화소를 거리·알파 가중 평균한다(UE
    // GetFilledFromNearNeighbors — 같은 순서로 더한다). 없으면 흰색 그대로다.
    void FilledFromNeighbors(int64 centerX, int64 centerY, Channel* out) noexcept {
        const int64 lowX = centerX - InfillRadius > 0 ? centerX - InfillRadius : 0;
        const int64 lowY = centerY - InfillRadius > 0 ? centerY - InfillRadius : 0;
        const int64 highX =
            centerX + InfillRadius < width_ - 1 ? centerX + InfillRadius : width_ - 1;
        const int64 highY =
            centerY + InfillRadius < height_ - 1 ? centerY + InfillRadius : height_ - 1;
        float totalWeight = 0.0f;
        float total[Channels] = {};
        for (int64 y = lowY; y <= highY; ++y) {
            const float* rowWeights = InfillWeights[y > centerY ? y - centerY : centerY - y];
            for (int64 x = lowX; x <= highX; ++x) {
                const Channel* pixel = Pixel(x, y);
                const float alpha = TLayout::ToLinear(pixel[Alpha], true);
                if (alpha == 0.0f) {
                    continue;
                }

                float weight = rowWeights[x > centerX ? x - centerX : centerX - x];
                weight *= alpha;
                totalWeight += weight;
                for (usize channel = 0; channel < Channels; ++channel) {
                    const float weighted =
                        weight * TLayout::ToLinear(pixel[channel], channel == Alpha);
                    total[channel] += weighted;
                }
            }
        }

        if (totalWeight == 0.0f) {
            for (usize channel = 0; channel < Alpha; ++channel) {
                out[channel] = TLayout::Max;
            }

            out[Alpha] = 0;
            return;
        }

        const float scale = 1.0f / totalWeight;
        for (usize channel = 0; channel < Alpha; ++channel) {
            const float color = total[channel] * scale;
            out[channel] = TLayout::FromLinear(color);
        }

        out[Alpha] = 0;
    }

    Result<void> FillFromNearNeighbors(Allocator& scratch) noexcept {
        Array<Channel> row(scratch);
        if (auto sized = row.TryResize(static_cast<usize>(width_) * Channels); !sized) {
            return sized;
        }

        for (int64 y = 0; y < height_; ++y) {
            for (int64 x = 0; x < width_; ++x) {
                Channel* target = row.Data() + x * static_cast<int64>(Channels);
                const Channel* pixel = Pixel(x, y);
                if (IsWhiteWithZeroAlpha(pixel)) {
                    FilledFromNeighbors(x, y, target);
                } else {
                    for (usize channel = 0; channel < Channels; ++channel) {
                        target[channel] = pixel[channel];
                    }
                }
            }

            Channel* line = Pixel(0, y);
            for (usize index = 0; index < row.Size(); ++index) {
                line[index] = row[index];
            }
        }

        return Result<void>::Success();
    }

    // 행 하나의 흰색 투명 구간을 양옆 색으로 채운다(UE ProcessHorizontalRow). 행 전체가 흰색
    // 투명이면 거짓이다.
    bool ProcessRow(int64 y) noexcept {
        int64 x = 0;
        if (IsWhiteWithZeroAlpha(Pixel(0, y))) {
            while (IsWhiteWithZeroAlpha(Pixel(x, y))) {
                ++x;
                if (x == width_) {
                    return false;
                }
            }

            for (int64 fill = 0; fill < x; ++fill) {
                SetColorWithZeroAlpha(Pixel(fill, y), Pixel(x, y));
            }
        }

        for (;;) {
            while (!IsWhiteWithZeroAlpha(Pixel(x, y))) {
                ++x;
                if (x == width_) {
                    return true;
                }
            }

            const int64 firstTransparent = x;
            while (IsWhiteWithZeroAlpha(Pixel(x, y))) {
                ++x;
                if (x == width_) {
                    for (int64 fill = firstTransparent; fill < width_; ++fill) {
                        SetColorWithZeroAlpha(Pixel(fill, y), Pixel(firstTransparent - 1, y));
                    }

                    return true;
                }
            }

            // [firstTransparent, x) 양쪽에 색이 있다: 왼쪽 절반은 왼쪽 색, 오른쪽 절반은 오른쪽 색.
            const int64 middle = (firstTransparent + x) / 2;
            for (int64 fill = firstTransparent; fill < middle; ++fill) {
                SetColorWithZeroAlpha(Pixel(fill, y), Pixel(firstTransparent - 1, y));
            }

            for (int64 fill = middle; fill < x; ++fill) {
                SetColorWithZeroAlpha(Pixel(fill, y), Pixel(x, y));
            }
        }
    }

    // from 행의 색을 to 행에 복사한다(알파는 그대로 — UE FillRowColorPixels).
    void CopyRowColor(int64 from, int64 to) noexcept {
        for (int64 x = 0; x < width_; ++x) {
            const Channel* source = Pixel(x, from);
            Channel* target = Pixel(x, to);
            for (usize channel = 0; channel < Alpha; ++channel) {
                target[channel] = source[channel];
            }
        }
    }

    Channel* data_;
    int64 width_;
    int64 height_;
};

// TGA 머리 칸이다(color map type·image type·bits per pixel).
constexpr usize TgaHeaderBytes = 18;
constexpr usize TgaColorMapTypeOffset = 1;
constexpr usize TgaImageTypeOffset = 2;
constexpr usize TgaBitsPerPixelOffset = 16;
constexpr uint8 TgaColorMappedImage = 1;
constexpr uint8 TgaGrayImage = 3;
constexpr uint8 TgaEightBits = 8;

// 한 파일 Cube 배치의 칸 수와 면 칸 위치다(12.3절 표, 면 순서 +X −X +Y −Y +Z −Z).
struct CubeCell {
    uint32 column = 0;
    uint32 row = 0;
};

// 배치 하나의 면 수다.
constexpr usize CubeLayoutFaces = 6;

struct CubeLayoutCells {
    uint32 columns = 0;
    uint32 rows = 0;
    CubeCell faces[CubeLayoutFaces];
};

constexpr CubeLayoutCells HorizontalCrossCells{
    4, 3, {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}}};
constexpr CubeLayoutCells VerticalCrossCells{
    3, 4, {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {1, 3}}};
constexpr CubeLayoutCells Strip6Cells{6, 1, {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}};
// VerticalCross에서 180° 돌려 담긴 면(−Z)의 번호다.
constexpr usize VerticalCrossRotatedFace = 5;

} // namespace

uint8 LinearToSrgb8(float value) noexcept {
    // stbir__linear_to_srgb_uchar_fast와 같다(NaN은 아래 비교로 최소값이 된다).
    const float minimum = platform::BitCast<float>(LinearToSrgbMinBits);
    const float almostOne = platform::BitCast<float>(LinearToSrgbAlmostOneBits);
    if (!(value > minimum)) {
        value = minimum;
    }

    if (value > almostOne) {
        value = almostOne;
    }

    const uint32 bits = platform::BitCast<uint32>(value);
    const uint32 entry = LinearToSrgbTable[(bits - LinearToSrgbMinBits) >> LinearToSrgbIndexShift];
    const uint32 bias = (entry >> LinearToSrgbBiasShift) << LinearToSrgbBiasScale;
    const uint32 scale = entry & LinearToSrgbScaleMask;
    const uint32 mantissa = (bits >> LinearToSrgbMantissaShift) & LinearToSrgbMantissaMask;
    return static_cast<uint8>((bias + scale * mantissa) >> LinearToSrgbResultShift);
}

bool IsGrayConvertible(const image::DecodedImage& image) noexcept {
    const uint8* pixels = image.pixels.Data();
    const usize texels = image.pixels.Size() / (image.format == TextureSourceFormat::RGBA8 ? 4 : 2);
    if (image.format == TextureSourceFormat::RGBA8) {
        for (usize index = 0; index < texels; ++index) {
            const uint8* texel = pixels + index * 4;
            if (texel[3] != 255 || texel[0] != texel[2] || texel[1] != texel[2]) {
                return false;
            }
        }

        return true;
    }

    if (image.format == TextureSourceFormat::GA8) {
        for (usize index = 0; index < texels; ++index) {
            if (pixels[index * 2 + 1] != 255) {
                return false;
            }
        }

        return true;
    }

    return false;
}

void ConvertToG8(image::DecodedImage& image) noexcept {
    const usize stride = image.format == TextureSourceFormat::RGBA8 ? 4 : 2;
    ROCKS_ASSERT(image.format == TextureSourceFormat::RGBA8 ||
                 image.format == TextureSourceFormat::GA8);
    const usize texels = image.pixels.Size() / stride;
    uint8* pixels = image.pixels.Data();
    for (usize index = 0; index < texels; ++index) {
        pixels[index] = pixels[index * stride];
    }

    // 줄이기는 할당하지 않는다.
    (void)image.pixels.TryResize(texels);
    image.format = TextureSourceFormat::G8;
}

Result<void> FillTransparentTexels(image::DecodedImage& image, TextureImportPngInfill mode,
                                   Allocator& scratch) noexcept {
    if (mode == TextureImportPngInfill::Never) {
        return Result<void>::Success();
    }

    const bool always = mode == TextureImportPngInfill::Always;
    const int64 width = image.width;
    const int64 height = static_cast<int64>(image.height) * image.numSlices;
    switch (image.format) {
    case TextureSourceFormat::RGBA8: {
        TransparentFill<PixelLayout<uint8, 4>> fill(image.pixels.Data(), width, height);
        return fill.Process(always, scratch);
    }
    case TextureSourceFormat::GA8: {
        TransparentFill<PixelLayout<uint8, 2>> fill(image.pixels.Data(), width, height);
        return fill.Process(always, scratch);
    }
    case TextureSourceFormat::RGBA16: {
        TransparentFill<PixelLayout<uint16, 4>> fill(reinterpret_cast<uint16*>(image.pixels.Data()),
                                                     width, height);
        return fill.Process(always, scratch);
    }
    case TextureSourceFormat::GA16: {
        TransparentFill<PixelLayout<uint16, 2>> fill(reinterpret_cast<uint16*>(image.pixels.Data()),
                                                     width, height);
        return fill.Process(always, scratch);
    }
    default:
        return Result<void>::Success();
    }
}

bool IsTgaColorMapped8(Span<const uint8> file) noexcept {
    return file.Size() >= TgaHeaderBytes && file[TgaColorMapTypeOffset] == 1 &&
           file[TgaImageTypeOffset] == TgaColorMappedImage &&
           file[TgaBitsPerPixelOffset] == TgaEightBits;
}

bool IsTgaGray8(Span<const uint8> file) noexcept {
    return file.Size() >= TgaHeaderBytes && file[TgaColorMapTypeOffset] == 0 &&
           file[TgaImageTypeOffset] == TgaGrayImage && file[TgaBitsPerPixelOffset] == TgaEightBits;
}

Result<bool> ExtractCubeLayout(const image::DecodedImage& image, TextureCubeLayout layout,
                               Array<uint8>& out, uint32& face) noexcept {
    const CubeLayoutCells* cells =
        layout == TextureCubeLayout::HorizontalCross ? &HorizontalCrossCells
        : layout == TextureCubeLayout::VerticalCross ? &VerticalCrossCells
        : layout == TextureCubeLayout::Strip6        ? &Strip6Cells
                                                     : nullptr;
    if (cells == nullptr || image.numSlices != 1 || image.width % cells->columns != 0 ||
        image.height % cells->rows != 0 ||
        image.width / cells->columns != image.height / cells->rows || image.width == 0) {
        return Result<bool>::Success(false);
    }

    face = image.width / cells->columns;
    const usize texel = TextureSourceTexelBytes(image.format);
    const usize faceRow = usize{face} * texel;
    const usize imageRow = usize{image.width} * texel;
    if (auto sized = out.TryResize(faceRow * face * CubeLayoutFaces); !sized) {
        return Result<bool>::Failure(sized.Error());
    }

    uint8* target = out.Data();
    for (usize index = 0; index < CubeLayoutFaces; ++index) {
        const CubeCell cell = cells->faces[index];
        const bool rotated =
            layout == TextureCubeLayout::VerticalCross && index == VerticalCrossRotatedFace;
        for (uint32 y = 0; y < face; ++y) {
            for (uint32 x = 0; x < face; ++x) {
                // 180° 회전은 (x, y) → (face − 1 − x, face − 1 − y)다.
                const uint32 sourceX = rotated ? face - 1 - x : x;
                const uint32 sourceY = rotated ? face - 1 - y : y;
                const uint8* source = image.pixels.Data() +
                                      (usize{cell.row} * face + sourceY) * imageRow +
                                      (usize{cell.column} * face + sourceX) * texel;
                for (usize byte = 0; byte < texel; ++byte) {
                    target[byte] = source[byte];
                }

                target += texel;
            }
        }
    }

    return Result<bool>::Success(true);
}

Result<bool> ExtractVolumeTiles(const image::DecodedImage& image, uint32 tileX, uint32 tileY,
                                uint32 maxSlices, Array<uint8>& out, uint32& slices) noexcept {
    if (tileX == 0 || tileY == 0 || image.numSlices != 1 || image.width % tileX != 0 ||
        image.height % tileY != 0) {
        return Result<bool>::Success(false);
    }

    const uint32 columns = image.width / tileX;
    const uint32 rows = image.height / tileY;
    if (uint64{columns} * rows > maxSlices) {
        return Result<bool>::Success(false);
    }

    slices = columns * rows;
    const usize texel = TextureSourceTexelBytes(image.format);
    const usize imageRow = usize{image.width} * texel;
    const usize tileRow = usize{tileX} * texel;
    if (auto sized = out.TryResize(image.pixels.Size()); !sized) {
        return Result<bool>::Failure(sized.Error());
    }

    uint8* target = out.Data();
    for (uint32 slice = 0; slice < slices; ++slice) {
        const usize originX = usize{slice % columns} * tileX;
        const usize originY = usize{slice / columns} * tileY;
        for (uint32 y = 0; y < tileY; ++y) {
            const uint8* source = image.pixels.Data() + (originY + y) * imageRow + originX * texel;
            platform::MemoryCopy(target, source, tileRow);
            target += tileRow;
        }
    }

    return Result<bool>::Success(true);
}

} // namespace rocks::texture_import
