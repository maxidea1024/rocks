#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <Texture/TextureBlockDecode.h>

#include <string.h>

// 블록 형식 CPU 디코더 시험이다(Docs/Design/AssetSystem 12.17.7). 손으로 만든 블록마다 D3D11
// BC7·BC6H 정수식으로 계산한 값을 단언한다(BC7 mode 1·4·6·예약, BC6H mode 11·12·예약). BC1–BC5
// 디코더는 TextureTools의 NVTT golden 시험이 함께 본다.
using namespace rocks;
namespace texture_block_decode_tests_file {

namespace {

constexpr uint32 Bptc = 16;
constexpr uint16 HalfOne = 0x3C00;

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

} // namespace

// 기준 디코더의 손으로 만든 블록이다(D3D11 BC7 mode 6·예약 모드, BC6H mode 11·예약 모드).
TEST(TextureBlockDecode_KnownBlocks) {
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

// BC3 색 블록은 c0 ≤ c1이어도 4색이다(D3D 명세 — BC1만 3색 + 투명 검정). 같은 색 블록을 BC1로 풀면
// index 3이 투명 검정이다.
TEST(TextureBlockDecode_Bc3ColorAlwaysFourColor) {
    // 색 블록: c0 = 0x001F(파랑) < c1 = 0xF800(빨강), 모든 index 3. 알파 블록: alpha0 = alpha1 =
    // 255, index 0.
    const uint8 color[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8 bc3[Bptc] = {0xFF, 0xFF};
    memcpy(bc3 + 8, color, sizeof(color));
    uint8 texels[TextureBlockRgbaBytes];
    DecodeTextureBlock(TextureBlockFormat::Bc3, bc3, texels);
    // 4색 index 3 = (2·c1 + c0) / 3 = (170, 0, 85), 알파는 알파 블록(255).
    CHECK(texels[0] == 170 && texels[1] == 0 && texels[2] == 85 && texels[3] == 255);
    DecodeTextureBlock(TextureBlockFormat::Bc1, color, texels);
    CHECK(texels[0] == 0 && texels[1] == 0 && texels[2] == 0 && texels[3] == 0);
}

// 크기 질의: 블록 byte와 출력 byte(블록보다 작은 mip도 블록 하나, 크기 0 거부).
TEST(TextureBlockDecode_Sizes) {
    CHECK(TextureBlockBytes(TextureBlockFormat::Bc1) == 8 &&
          TextureBlockBytes(TextureBlockFormat::Bc4) == 8);
    CHECK(TextureBlockBytes(TextureBlockFormat::Bc3) == 16 &&
          TextureBlockBytes(TextureBlockFormat::Bc5) == 16 &&
          TextureBlockBytes(TextureBlockFormat::Bc6h) == 16 &&
          TextureBlockBytes(TextureBlockFormat::Bc7) == 16);
    auto bytes = TextureBlockOutputBytes(TextureBlockFormat::Bc7, 5, 3, 2);
    CHECK(bytes && bytes.Value() == 2 * 1 * 2 * 16);
    CHECK(TextureBlockOutputBytes(TextureBlockFormat::Bc1, 0, 4, 1).Error() ==
          ErrorCode::InvalidArgument);
    uint8 palette[TextureBlockAlphaPaletteSize];
    TextureBlockAlphaPalette(200, 10, palette);
    CHECK(palette[0] == 200 && palette[1] == 10 && palette[2] == (6 * 200 + 10) / 7 &&
          palette[7] == (200 + 6 * 10) / 7);
    TextureBlockAlphaPalette(10, 200, palette);
    CHECK(palette[2] == (4 * 10 + 200) / 5 && palette[6] == 0 && palette[7] == 255);
}

} // namespace texture_block_decode_tests_file
