// ISPC 타깃 간 결정성 probe다(Docs/Design/AssetSystem 12.17.6 10). run.py가 kernel.ispc를 타깃마다 따로
// 컴파일해 이 원본·ispc_texcomp.cpp와 링크한다. 두 해시를 출력한다:
// - golden: 측정 그림(IsMeasured)의 BC7(RGB·알파 프로필)·BC6H 출력 FNV-1a 64를 EngineTests
//   TextureBlockBptc_OutputHashGolden과 같은 순서로 이은 값(Rocks 빌드 출력과 대조한다).
// - full: 모든 그림(해시만인 드문 경로 256 × 256 포함)과 퇴화 HDR 입력(0·65504·비정규·두 값 — BC6H 입력 정리 뒤
//   값)의 출력 해시(타깃끼리만 대조한다 — 재시작 판정처럼 퇴화 공분산에 민감한 경로를 덮는다).
// Rocks 빌드 밖에서 컴파일하므로 Rocks 헤더를 쓰지 않는다(시험 그림 생성기 BlockGoldenImages.h는 기본 타입만
// 쓴다).
//
// 사용: IspcDeterminismProbe [출력 파일] — 출력 파일을 주면 모든 블록 byte도 쓴다(타깃 사이 diff용).

#include "BlockGoldenImages.h"
#include "ispc_texcomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

namespace golden = rocks::test::block_golden;

constexpr unsigned Channels = 4;
constexpr unsigned HalfTexelBytes = 8;
constexpr unsigned BlockBytes = 16;
constexpr unsigned BlockTexels = 16;
// 퇴화 HDR 그림의 크기다(블록 넷).
constexpr unsigned DegenerateSize = 8;
// half bit 값이다: 0, 가장 큰 유한값, 1.0, 가장 작은 비정규값, 2.0.
constexpr uint16_t HalfZero = 0x0000;
constexpr uint16_t HalfMax = 0x7BFF;
constexpr uint16_t HalfOne = 0x3C00;
constexpr uint16_t HalfTiny = 0x0001;
constexpr uint16_t HalfTwo = 0x4000;

// Rocks GenericPlatformMath::FloatToHalf와 같은 IEEE RTNE 변환이다(시험의 Float16::FromFloat와 같은 bit).
uint16_t FloatToHalf(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const uint32_t magnitude = bits & 0x7fffffffu;
    if (magnitude >= 0x7f800000u) {
        if (magnitude == 0x7f800000u) {
            return static_cast<uint16_t>(sign | 0x7c00u);
        }

        return static_cast<uint16_t>(sign | 0x7e00u | ((magnitude >> 13) & 0x3ffu));
    }

    if (magnitude >= 0x477ff000u) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }

    if (magnitude < 0x38800000u) {
        const uint32_t exponent = magnitude >> 23;
        if (exponent < 102u) {
            return sign;
        }

        const uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
        const uint32_t shift = 126u - exponent;
        uint32_t result = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (result & 1u) != 0)) {
            ++result;
        }

        return static_cast<uint16_t>(sign | result);
    }

    uint32_t result = (magnitude - 0x38000000u) >> 13;
    const uint32_t remainder = magnitude & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0)) {
        ++result;
    }

    return static_cast<uint16_t>(sign | result);
}

void StoreHalf(unsigned char* texel, unsigned channel, uint16_t bits) {
    texel[channel * 2] = static_cast<unsigned char>(bits & 0xFF);
    texel[channel * 2 + 1] = static_cast<unsigned char>(bits >> 8);
}

// 시험 HdrFrom과 같은 HDR 그림이다(채널마다 v² × 배율, 알파 1.0).
void HdrFrom(const unsigned char* rgba, unsigned texels, unsigned char* out) {
    const float scale[3] = {64.0f, 4.0f, 0.25f};
    for (unsigned i = 0; i < texels; ++i) {
        for (unsigned c = 0; c < Channels; ++c) {
            const float unit = static_cast<float>(rgba[i * Channels + c]) / 255.0f;
            const float value = c == 3 ? 1.0f : unit * unit * scale[c];
            StoreHalf(out + i * HalfTexelBytes, c, FloatToHalf(value));
        }
    }
}

// 퇴화 HDR 그림이다(블록마다 다른 퇴화 — 0, 65504, 비정규 한 단계와 0, 1.0과 2.0 두 값). Rocks BC6H 입력 정리
// 뒤에 나올 수 있는 값만 쓴다(NaN·음수·Inf는 정리가 0·65504로 바꾼다).
void Degenerate(unsigned char* out) {
    for (unsigned y = 0; y < DegenerateSize; ++y) {
        for (unsigned x = 0; x < DegenerateSize; ++x) {
            const unsigned block = (y / 4) * 2 + x / 4;
            const unsigned t = (y % 4) * 4 + x % 4;
            uint16_t value = HalfZero;
            if (block == 1) {
                value = HalfMax;
            } else if (block == 2) {
                value = t % 2 == 0 ? HalfTiny : HalfZero;
            } else if (block == 3) {
                value = t < BlockTexels / 2 ? HalfOne : HalfTwo;
            }

            unsigned char* texel = out + (y * DegenerateSize + x) * HalfTexelBytes;
            for (unsigned c = 0; c < 3; ++c) {
                StoreHalf(texel, c, value);
            }

            StoreHalf(texel, 3, HalfOne);
        }
    }
}

struct Profiles {
    bc7_enc_settings basic{};
    bc7_enc_settings alpha{};
    bc6h_enc_settings bc6h{};
};

// 그림 하나의 세 출력(BC7 RGB·BC7 알파·BC6H)을 hash에 잇는다.
unsigned long long EncodeAll(const Profiles& profiles, unsigned char* rgba, unsigned char* hdr, unsigned sizeX,
                             unsigned sizeY, unsigned char* blocks, FILE* dump, unsigned long long hash) {
    const unsigned outputBytes = sizeX * sizeY / BlockTexels * BlockBytes;
    rgba_surface ldr{rgba, static_cast<int32_t>(sizeX), static_cast<int32_t>(sizeY), static_cast<int32_t>(sizeX * Channels)};
    rgba_surface wide{hdr, static_cast<int32_t>(sizeX), static_cast<int32_t>(sizeY),
                      static_cast<int32_t>(sizeX * HalfTexelBytes)};
    bc7_enc_settings basic = profiles.basic;
    bc7_enc_settings alpha = profiles.alpha;
    bc6h_enc_settings bc6h = profiles.bc6h;
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 0) {
            CompressBlocksBC7(&ldr, blocks, &basic);
        } else if (pass == 1) {
            CompressBlocksBC7(&ldr, blocks, &alpha);
        } else {
            CompressBlocksBC6H(&wide, blocks, &bc6h);
        }

        hash = golden::Fnv1a64(blocks, outputBytes, hash);
        if (dump != nullptr) {
            fwrite(blocks, 1, outputBytes, dump);
        }
    }

    return hash;
}

} // namespace

int main(int argc, char** argv) {
    FILE* dump = argc > 1 ? fopen(argv[1], "wb") : nullptr;
    if (argc > 1 && dump == nullptr) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    static unsigned char rgba[golden::MaxTexels * Channels];
    static unsigned char hdr[golden::MaxTexels * HalfTexelBytes];
    static unsigned char blocks[golden::MaxTexels];
    Profiles profiles;
    GetProfile_basic(&profiles.basic);
    GetProfile_alpha_basic(&profiles.alpha);
    GetProfile_bc6h_basic(&profiles.bc6h);
    unsigned long long measured = golden::FnvOffset;
    unsigned long long full = golden::FnvOffset;
    for (const golden::Case& one : golden::Cases) {
        // 4의 배수가 아닌 가장자리 그림은 Rocks 감싸기의 채움을 거치므로 여기서는 뺀다(채움은 EngineTests가 본다).
        if (one.sizeX % 4 != 0 || one.sizeY % 4 != 0) {
            continue;
        }

        golden::Generate(one, rgba);
        HdrFrom(rgba, one.sizeX * one.sizeY, hdr);
        if (golden::IsMeasured(one)) {
            measured = EncodeAll(profiles, rgba, hdr, one.sizeX, one.sizeY, blocks, nullptr, measured);
        }

        full = EncodeAll(profiles, rgba, hdr, one.sizeX, one.sizeY, blocks, dump, full);
    }

    for (unsigned i = 0; i < DegenerateSize * DegenerateSize * Channels; ++i) {
        rgba[i] = static_cast<unsigned char>(i % 4 == 3 ? 255 : 128);
    }

    Degenerate(hdr);
    full = EncodeAll(profiles, rgba, hdr, DegenerateSize, DegenerateSize, blocks, dump, full);
    if (dump != nullptr) {
        fclose(dump);
    }

    printf("golden %016llx\nfull %016llx\n", measured, full);
    return 0;
}
