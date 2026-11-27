#include "TextureBlockBptc.h"
#include "TextureBlockBptcTables.h"

// BC6H·BC7 기준 디코더다(Docs/Design/AssetSystem 12.17.6 5·9 — 시험·품질 측정용). D3D11 형식의 정수
// 절차를 Microsoft DirectXTex BC6HBC7.cpp(D3DX_BC6H::Decode·D3DX_BC7::Decode, MIT)와 같은 순서로
// 옮겼다. 표는 TextureBlockBptcTables.h다(인코더 커널의 표와 따로 — 디코더가 인코더와 독립이다).
namespace rocks::texture_block_detail {

namespace {

// 유니티 빌드에서 다른 번역 단위의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace bptc_decode {

constexpr uint32 Channels = 4;
constexpr uint32 ColorChannels = 3;
constexpr uint32 MaxSubsets = 3;
// BC7 p-bit 수의 최대(mode 0 — 끝점 여섯에 하나씩)다.
constexpr uint32 MaxPBits = 6;
// BC6H 모드 번호는 2 bit로 시작하고, 0·1이 아니면 3 bit를 더 읽는다.
constexpr uint32 Bc6hShortModeBits = 2;
constexpr uint32 Bc6hLongModeBits = 3;
// unsigned BC6H 마무리: 크기를 31/64로 줄인다(DirectXTex FinishUnquantize).
constexpr int Bc6hFinishScale = 31;
constexpr uint32 Bc6hFinishShift = 6;
// unsigned BC6H 역양자화: 16 bit 눈금의 가운데(0x8000)를 더해 bit 수만큼 줄인다.
constexpr uint32 Bc6hUnquantizeShift = 16;
constexpr int Bc6hUnquantizeRound = 0x8000;
// 보간 가중치의 합과 반올림이다(BC6H·BC7 공통 — 64 단계, 32를 더해 6 bit 내림).
constexpr int WeightMax = 64;
constexpr int WeightRound = 32;
constexpr uint32 WeightShift = 6;
constexpr int Weights2[4] = {0, 21, 43, 64};
constexpr int Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
// half 1.0이다.
constexpr uint16 HalfOne = 0x3C00;
// BC6H 머리 bit 수다(2 subset 모드·1 subset 모드).
constexpr uint32 Bc6hHeaderBitsTwo = 82;
constexpr uint32 Bc6hHeaderBitsOne = 65;
// BC6H 모드 번호(2 bit 또는 5 bit)는 32개이고 그중 14개만 쓴다.
constexpr uint32 Bc6hModeCodes = 32;
constexpr uint32 Bc6hModes = 14;
// 쓰지 않는 BC6H 모드 번호의 표시다.
constexpr int Bc6hNoMode = -1;

// 블록 bit를 낮은 bit부터 읽는다.
struct BitReader {
    const uint8* bytes;
    uint32 position = 0;

    uint32 Bit() noexcept {
        const uint32 value = (bytes[position >> 3] >> (position & 7)) & 1u;
        ++position;
        return value;
    }

    uint32 Bits(uint32 count) noexcept {
        uint32 value = 0;
        for (uint32 i = 0; i < count; ++i) {
            value |= Bit() << i;
        }

        return value;
    }
};

const int* WeightsOf(uint32 precision) noexcept {
    return precision == 2 ? Weights2 : precision == 3 ? Weights3 : Weights4;
}

int Interpolate(int a, int b, uint32 index, uint32 precision) noexcept {
    const int weight = WeightsOf(precision)[index];
    return (a * (WeightMax - weight) + b * weight + WeightRound) >> WeightShift;
}

// texel이 subset의 anchor인지다(anchor index는 한 bit 적게 저장된다).
bool IsAnchor(uint32 subsets, uint32 shape, uint32 texel) noexcept {
    if (texel == 0) {
        return true;
    }

    for (uint32 subset = 1; subset < subsets; ++subset) {
        if (BptcAnchors[subsets - 2][shape][subset - 1] == texel) {
            return true;
        }
    }

    return false;
}

uint32 SubsetOf(uint32 subsets, uint32 shape, uint32 texel) noexcept {
    return subsets == 1 ? 0 : BptcPartitions[subsets - 2][shape][texel];
}

// --- BC7 ---

// DirectXTex D3DX_BC7::ms_aInfo와 같은 값이다.
struct Bc7Mode {
    uint32 subsets;
    uint32 partitionBits;
    uint32 pBits;
    uint32 rotationBits;
    uint32 indexModeBits;
    uint32 indexPrecision;
    uint32 indexPrecision2;
    uint8 precision[Channels];
    uint8 precisionWithP[Channels];
};

constexpr uint32 Bc7ModeCount = 8;

constexpr Bc7Mode Bc7Modes[Bc7ModeCount] = {
    {3, 4, 6, 0, 0, 3, 0, {4, 4, 4, 0}, {5, 5, 5, 0}},
    {2, 6, 2, 0, 0, 3, 0, {6, 6, 6, 0}, {7, 7, 7, 0}},
    {3, 6, 0, 0, 0, 2, 0, {5, 5, 5, 0}, {5, 5, 5, 0}},
    {2, 6, 4, 0, 0, 2, 0, {7, 7, 7, 0}, {8, 8, 8, 0}},
    {1, 0, 0, 2, 1, 2, 3, {5, 5, 5, 6}, {5, 5, 5, 6}},
    {1, 0, 0, 2, 0, 2, 2, {7, 7, 7, 8}, {7, 7, 7, 8}},
    {1, 0, 2, 0, 0, 4, 0, {7, 7, 7, 7}, {8, 8, 8, 8}},
    {2, 6, 4, 0, 0, 2, 0, {5, 5, 5, 5}, {6, 6, 6, 6}},
};

constexpr uint32 ByteBits = 8;

uint32 Unquantize7(uint32 value, uint32 precision) noexcept {
    const uint32 shifted = (value << (ByteBits - precision)) & 0xFFu;
    return shifted | (shifted >> precision);
}

void DecodeBc7(const uint8* block, uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    BitReader reader{block};
    uint32 mode = 0;
    while (mode < Bc7ModeCount && reader.Bit() == 0) {
        ++mode;
    }

    if (mode >= Bc7ModeCount) {
        for (uint8& value : texels) {
            value = 0;
        }

        return;
    }

    const Bc7Mode& info = Bc7Modes[mode];
    const uint32 shape = reader.Bits(info.partitionBits);
    const uint32 rotation = reader.Bits(info.rotationBits);
    const uint32 indexMode = reader.Bits(info.indexModeBits);
    const uint32 endpoints = info.subsets * 2;
    uint32 colors[MaxSubsets * 2][Channels] = {};
    for (uint32 channel = 0; channel < Channels; ++channel) {
        for (uint32 e = 0; e < endpoints; ++e) {
            colors[e][channel] =
                info.precision[channel] != 0 ? reader.Bits(info.precision[channel]) : 255u;
        }
    }

    uint32 pbits[MaxPBits] = {};
    for (uint32 i = 0; i < info.pBits; ++i) {
        pbits[i] = reader.Bit();
    }

    for (uint32 e = 0; e < endpoints; ++e) {
        const uint32 p = info.pBits == 0 ? 0 : pbits[e * info.pBits / endpoints];
        for (uint32 channel = 0; channel < Channels; ++channel) {
            if (info.pBits != 0 && info.precision[channel] != info.precisionWithP[channel]) {
                colors[e][channel] = (colors[e][channel] << 1) | p;
            }

            colors[e][channel] = info.precisionWithP[channel] != 0
                                     ? Unquantize7(colors[e][channel], info.precisionWithP[channel])
                                     : 255u;
        }
    }

    uint32 primary[TextureBlockTexels] = {};
    uint32 secondary[TextureBlockTexels] = {};
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 bits =
            IsAnchor(info.subsets, shape, i) ? info.indexPrecision - 1 : info.indexPrecision;
        primary[i] = reader.Bits(bits);
    }

    if (info.indexPrecision2 != 0) {
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            secondary[i] = reader.Bits(i == 0 ? info.indexPrecision2 - 1 : info.indexPrecision2);
        }
    }

    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 subset = SubsetOf(info.subsets, shape, i);
        const uint32* a = colors[subset * 2];
        const uint32* b = colors[subset * 2 + 1];
        uint32 colorIndex = primary[i];
        uint32 colorPrecision = info.indexPrecision;
        uint32 alphaIndex = primary[i];
        uint32 alphaPrecision = info.indexPrecision;
        if (info.indexPrecision2 != 0) {
            if (indexMode == 0) {
                alphaIndex = secondary[i];
                alphaPrecision = info.indexPrecision2;
            } else {
                colorIndex = secondary[i];
                colorPrecision = info.indexPrecision2;
            }
        }

        uint32 texel[Channels];
        for (uint32 channel = 0; channel < ColorChannels; ++channel) {
            texel[channel] = static_cast<uint32>(Interpolate(static_cast<int>(a[channel]),
                                                             static_cast<int>(b[channel]),
                                                             colorIndex, colorPrecision));
        }

        texel[3] = static_cast<uint32>(Interpolate(static_cast<int>(a[3]), static_cast<int>(b[3]),
                                                   alphaIndex, alphaPrecision));
        if (rotation != 0) {
            const uint32 swapped = texel[rotation - 1];
            texel[rotation - 1] = texel[3];
            texel[3] = swapped;
        }

        for (uint32 channel = 0; channel < Channels; ++channel) {
            texels[i * Channels + channel] = static_cast<uint8>(texel[channel]);
        }
    }
}

// --- BC6H ---

// DirectXTex D3DX_BC6H::ms_aInfo와 같은 값이다(precision[subset][끝점]은 RGB bit 수).
struct Bc6hMode {
    bool twoSubsets;
    bool transformed;
    uint32 indexPrecision;
    uint8 precision[2][2][3];
};

constexpr Bc6hMode Bc6hModeTable[Bc6hModes] = {
    {true, true, 3, {{{10, 10, 10}, {5, 5, 5}}, {{5, 5, 5}, {5, 5, 5}}}},
    {true, true, 3, {{{7, 7, 7}, {6, 6, 6}}, {{6, 6, 6}, {6, 6, 6}}}},
    {true, true, 3, {{{11, 11, 11}, {5, 4, 4}}, {{5, 4, 4}, {5, 4, 4}}}},
    {true, true, 3, {{{11, 11, 11}, {4, 5, 4}}, {{4, 5, 4}, {4, 5, 4}}}},
    {true, true, 3, {{{11, 11, 11}, {4, 4, 5}}, {{4, 4, 5}, {4, 4, 5}}}},
    {true, true, 3, {{{9, 9, 9}, {5, 5, 5}}, {{5, 5, 5}, {5, 5, 5}}}},
    {true, true, 3, {{{8, 8, 8}, {6, 5, 5}}, {{6, 5, 5}, {6, 5, 5}}}},
    {true, true, 3, {{{8, 8, 8}, {5, 6, 5}}, {{5, 6, 5}, {5, 6, 5}}}},
    {true, true, 3, {{{8, 8, 8}, {5, 5, 6}}, {{5, 5, 6}, {5, 5, 6}}}},
    {true, false, 3, {{{6, 6, 6}, {6, 6, 6}}, {{6, 6, 6}, {6, 6, 6}}}},
    {false, false, 4, {{{10, 10, 10}, {10, 10, 10}}, {{0, 0, 0}, {0, 0, 0}}}},
    {false, true, 4, {{{11, 11, 11}, {9, 9, 9}}, {{0, 0, 0}, {0, 0, 0}}}},
    {false, true, 4, {{{12, 12, 12}, {8, 8, 8}}, {{0, 0, 0}, {0, 0, 0}}}},
    {false, true, 4, {{{16, 16, 16}, {4, 4, 4}}, {{0, 0, 0}, {0, 0, 0}}}},
};

// 모드 번호 → 표 번호다(DirectXTex D3DX_BC6H::ms_aModeToInfo).
constexpr int Bc6hModeIndex[Bc6hModeCodes] = {
    0,          1,          2, 10,         Bc6hNoMode, Bc6hNoMode, 3, 11,
    Bc6hNoMode, Bc6hNoMode, 4, 12,         Bc6hNoMode, Bc6hNoMode, 5, 13,
    Bc6hNoMode, Bc6hNoMode, 6, Bc6hNoMode, Bc6hNoMode, Bc6hNoMode, 7, Bc6hNoMode,
    Bc6hNoMode, Bc6hNoMode, 8, Bc6hNoMode, Bc6hNoMode, Bc6hNoMode, 9, Bc6hNoMode};

int SignExtend(int value, uint32 bits) noexcept {
    const int sign = 1 << (bits - 1);
    return (value & sign) != 0 ? value | -(1 << bits) : value;
}

int Unquantize6(int value, uint32 bits) noexcept {
    constexpr uint32 FullBits = 15;
    constexpr int FullScale = 0xFFFF;
    if (bits >= FullBits || value == 0) {
        return value;
    }

    if (value == (1 << bits) - 1) {
        return FullScale;
    }

    return ((value << Bc6hUnquantizeShift) + Bc6hUnquantizeRound) >> bits;
}

void Opaque(uint16 (&halfs)[TextureBlockTexels * 4]) noexcept {
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        halfs[i * Channels] = 0;
        halfs[i * Channels + 1] = 0;
        halfs[i * Channels + 2] = 0;
        halfs[i * Channels + 3] = HalfOne;
    }
}

void DecodeBc6h(const uint8* block, uint16 (&halfs)[TextureBlockTexels * 4]) noexcept {
    BitReader reader{block};
    uint32 code = reader.Bits(Bc6hShortModeBits);
    if (code != 0 && code != 1) {
        code |= reader.Bits(Bc6hLongModeBits) << Bc6hShortModeBits;
    }

    const int index = Bc6hModeIndex[code];
    if (index == Bc6hNoMode) {
        Opaque(halfs);
        return;
    }

    const Bc6hMode& info = Bc6hModeTable[index];
    const Bc6hHeaderBit* header = Bc6hHeaders[index];
    // endpoint[subset][끝점][채널]이다.
    int endpoint[2][2][3] = {};
    uint32 shape = 0;
    const uint32 headerBits = info.twoSubsets ? Bc6hHeaderBitsTwo : Bc6hHeaderBitsOne;
    while (reader.position < headerBits) {
        const Bc6hHeaderBit field = header[reader.position];
        if (reader.Bit() == 0) {
            continue;
        }

        const int bit = 1 << field.bit;
        switch (field.field) {
        case Bc6hField::Shape:
            shape |= static_cast<uint32>(bit);
            break;
        case Bc6hField::RW:
            endpoint[0][0][0] |= bit;
            break;
        case Bc6hField::RX:
            endpoint[0][1][0] |= bit;
            break;
        case Bc6hField::RY:
            endpoint[1][0][0] |= bit;
            break;
        case Bc6hField::RZ:
            endpoint[1][1][0] |= bit;
            break;
        case Bc6hField::GW:
            endpoint[0][0][1] |= bit;
            break;
        case Bc6hField::GX:
            endpoint[0][1][1] |= bit;
            break;
        case Bc6hField::GY:
            endpoint[1][0][1] |= bit;
            break;
        case Bc6hField::GZ:
            endpoint[1][1][1] |= bit;
            break;
        case Bc6hField::BW:
            endpoint[0][0][2] |= bit;
            break;
        case Bc6hField::BX:
            endpoint[0][1][2] |= bit;
            break;
        case Bc6hField::BY:
            endpoint[1][0][2] |= bit;
            break;
        case Bc6hField::BZ:
            endpoint[1][1][2] |= bit;
            break;
        case Bc6hField::None:
        case Bc6hField::Mode:
            // 머리 안에 정의되지 않은 bit가 켜졌다(DirectXTex는 오류 색 — 기준 디코더는 검정).
            Opaque(halfs);
            return;
        }
    }

    const uint32 subsets = info.twoSubsets ? 2u : 1u;
    // 변환 모드는 끝점 0 밖의 값이 끝점 0에 대한 부호 있는 차이다(unsigned 형식도).
    if (info.transformed) {
        for (uint32 subset = 0; subset < subsets; ++subset) {
            for (uint32 channel = 0; channel < ColorChannels; ++channel) {
                if (subset != 0) {
                    endpoint[subset][0][channel] = SignExtend(endpoint[subset][0][channel],
                                                              info.precision[subset][0][channel]);
                }

                endpoint[subset][1][channel] =
                    SignExtend(endpoint[subset][1][channel], info.precision[subset][1][channel]);
            }
        }

        for (uint32 channel = 0; channel < ColorChannels; ++channel) {
            const int mask = (1 << info.precision[0][0][channel]) - 1;
            const int base = endpoint[0][0][channel];
            endpoint[0][1][channel] = (endpoint[0][1][channel] + base) & mask;
            endpoint[1][0][channel] = (endpoint[1][0][channel] + base) & mask;
            endpoint[1][1][channel] = (endpoint[1][1][channel] + base) & mask;
        }
    }

    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 bits =
            IsAnchor(subsets, shape, i) ? info.indexPrecision - 1 : info.indexPrecision;
        const uint32 texelIndex = reader.Bits(bits);
        const uint32 subset = SubsetOf(subsets, shape, i);
        for (uint32 channel = 0; channel < ColorChannels; ++channel) {
            const uint32 precision = info.precision[0][0][channel];
            const int a = Unquantize6(endpoint[subset][0][channel], precision);
            const int b = Unquantize6(endpoint[subset][1][channel], precision);
            const int value = Interpolate(a, b, texelIndex, info.indexPrecision);
            // unsigned 형식의 마무리: 크기를 31/64로 줄여 half bit로 쓴다.
            halfs[i * Channels + channel] =
                static_cast<uint16>((value * Bc6hFinishScale) >> Bc6hFinishShift);
        }

        halfs[i * Channels + 3] = HalfOne;
    }
}

} // namespace bptc_decode

} // namespace

void DecodeBc7Block(const uint8* block, uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    bptc_decode::DecodeBc7(block, texels);
}

void DecodeBc6hBlock(const uint8* block, uint16 (&halfs)[TextureBlockTexels * 4]) noexcept {
    bptc_decode::DecodeBc6h(block, halfs);
}

} // namespace rocks::texture_block_detail
