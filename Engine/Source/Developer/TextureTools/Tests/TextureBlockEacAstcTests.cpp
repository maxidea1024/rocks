#include "BlockGoldenImages.h"

#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureBlockEncode.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// EAC R11 인코더와 ASTC 감싸기 시험이다(Docs/Design/AssetSystem 12.17.8 9). EAC는 정확 최적 탐색이
// 256 base 전체 탐색과 같은 답을 내는지, 가장자리 가중치, 품질을 본다(디코드식·비트 배치는 Texture
// TextureBlockDecodeTests). ASTC는 블록 크기마다 왕복 품질, 채널 배치(RGB·RGBA·노멀 LA), sRGB
// profile, slice·pitch 독립성, 부동소수 환경, 동시 실행 결정성, 해시 golden을 본다.
using namespace rocks;
namespace texture_block_eac_astc_tests_file {

namespace {

namespace golden = test::block_golden;

constexpr uint32 Channels = 4;
constexpr uint32 EacBlock = 8;
// PSNR이 무한(오차 0)일 때 쓰는 값이다.
constexpr double PerfectPsnr = 99.0;

constexpr TextureBlockFormat AstcFormats[] = {
    TextureBlockFormat::Astc4x4, TextureBlockFormat::Astc6x6, TextureBlockFormat::Astc8x8,
    TextureBlockFormat::Astc10x10, TextureBlockFormat::Astc12x12};
// ASTC 크기별 왕복 PSNR 하한(dB)이다(값 잡음 32 × 32 — 2026-11-27 x64
// 측정 41.05·36.36·32.39·30.53·28.55에서 2 dB 여유).
constexpr double AstcMinimumPsnr[] = {39.0, 34.0, 30.0, 28.0, 26.0};

uint64 ReadBig64(const uint8* bytes) noexcept {
    uint64 value = 0;
    for (uint32 i = 0; i < EacBlock; ++i) {
        value = (value << 8) | bytes[i];
    }

    return value;
}

uint32 Target11(uint16 value) noexcept {
    return (uint32{value} * TextureEacR11Max + 32767) / 65535;
}

// (오차, 표, multiplier, base) 사전순 최적을 256 base 전체에서 찾는다(시험 기준).
struct Exhaustive {
    uint64 error = ~uint64{0};
    uint32 table = 0;
    uint32 multiplier = 0;
    uint32 base = 0;
};

Exhaustive SearchAll(const uint16 (&values)[TextureBlockTexels], uint16 mask) noexcept {
    Exhaustive best;
    for (uint32 table = 0; table < TextureEacTableCount; ++table) {
        for (uint32 multiplier = 0; multiplier < 16; ++multiplier) {
            for (uint32 base = 0; base < 256; ++base) {
                uint64 error = 0;
                for (uint32 i = 0; i < TextureBlockTexels && error <= best.error; ++i) {
                    if ((mask & (1u << i)) == 0) {
                        continue;
                    }

                    const int32 target = static_cast<int32>(Target11(values[i]));
                    uint32 nearest = ~0u;
                    for (uint32 k = 0; k < TextureEacTableSize; ++k) {
                        const int32 delta =
                            target - static_cast<int32>(TextureEacR11Value(
                                         base, multiplier, TextureEacModifiers[table][k]));
                        const uint32 squared = static_cast<uint32>(delta * delta);
                        nearest = squared < nearest ? squared : nearest;
                    }

                    error += nearest;
                }

                // 방문 순서가 사전순이므로 엄격히 작을 때만 바꾼다.
                if (error < best.error) {
                    best = Exhaustive{error, table, multiplier, base};
                }
            }
        }
    }

    return best;
}

uint64 BlockError(const uint8* block, const uint16 (&values)[TextureBlockTexels],
                  uint16 mask) noexcept {
    uint16 decoded[TextureBlockTexels];
    DecodeEacR11Block(block, decoded);
    uint64 error = 0;
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        if ((mask & (1u << i)) != 0) {
            const int64 delta = int64{Target11(values[i])} - decoded[i];
            error += static_cast<uint64>(delta * delta);
        }
    }

    return error;
}

void CheckMatchesExhaustive(const uint16 (&values)[TextureBlockTexels], uint16 mask) noexcept {
    uint8 block[EacBlock];
    EncodeEacR11Block(values, mask, block);
    const uint64 bits = ReadBig64(block);
    const Exhaustive expected = SearchAll(values, mask == 0 ? uint16{0xFFFF} : mask);
    CHECK(((bits >> 56) & 0xFF) == expected.base);
    CHECK(((bits >> 52) & 0xF) == expected.multiplier);
    CHECK(((bits >> 48) & 0xF) == expected.table);
    CHECK(BlockError(block, values, mask == 0 ? uint16{0xFFFF} : mask) == expected.error);
}

double Psnr(double sumSquared, double count, double peak) noexcept {
    if (sumSquared == 0.0) {
        return PerfectPsnr;
    }

    return 10.0 * log10(peak * peak * count / sumSquared);
}

bool GenerateRgba(const golden::Case& one, Array<uint8>& out) noexcept {
    if (!out.TryResize(usize{one.sizeX} * one.sizeY * Channels)) {
        return false;
    }

    golden::Generate(one, out.Data());
    return true;
}

const golden::Case& CaseNamed(const char* name) noexcept {
    for (const golden::Case& one : golden::Cases) {
        if (strcmp(one.name, name) == 0) {
            return one;
        }
    }

    return golden::Cases[0];
}

bool EncodeAstc(TextureBlockFormat format, const uint8* texels, uint32 x, uint32 y,
                const TextureBlockOptions& options, Array<uint8>& out, uint32 slices = 1,
                usize rowPitch = 0, usize slicePitch = 0) noexcept {
    auto bytes = TextureBlockOutputBytes(format, x, y, slices);
    if (!bytes || !out.TryResize(static_cast<usize>(bytes.Value()))) {
        return false;
    }

    const usize pitch = rowPitch == 0 ? usize{x} * Channels : rowPitch;
    const usize plane = slicePitch == 0 ? pitch * y : slicePitch;
    TextureBlockImage image;
    image.rgba =
        Span<const uint8>(texels, plane * (slices - 1) + pitch * (y - 1) + usize{x} * Channels);
    image.sizeX = x;
    image.sizeY = y;
    image.numSlices = slices;
    image.rowPitch = rowPitch;
    image.slicePitch = slicePitch;
    return static_cast<bool>(EncodeTextureBlocks(format, image, options, out.View()));
}

bool DecodeAstc(TextureBlockFormat format, const Array<uint8>& blocks, uint32 x, uint32 y,
                bool srgb, Array<uint8>& rgba, uint32 slices = 1) noexcept {
    if (!rgba.TryResize(usize{x} * y * Channels * slices)) {
        return false;
    }

    return static_cast<bool>(
        DecodeAstcBlocks(format, blocks.View(), x, y, slices, srgb, rgba.View()));
}

struct ConcurrentJob {
    TextureBlockFormat format;
    const uint8* texels;
    uint32 x;
    uint32 y;
    const Array<uint8>* expected;
    uint32 rounds;
    uint32 mismatches;
};

void EncodeRounds(void* raw) noexcept {
    ConcurrentJob& job = *static_cast<ConcurrentJob*>(raw);
    SystemAllocator allocator;
    for (uint32 round = 0; round < job.rounds; ++round) {
        Array<uint8> out(allocator);
        if (!EncodeAstc(job.format, job.texels, job.x, job.y, TextureBlockOptions{}, out) ||
            out.Size() != job.expected->Size() ||
            memcmp(out.Data(), job.expected->Data(), out.Size()) != 0) {
            ++job.mismatches;
        }
    }
}

} // namespace

// 정확 최적: 무작위·구조 블록에서 256 base 전체 탐색과 (표, multiplier, base)·오차가 같다(12.17.8 4
// — base 구간·가지치기·첫 후보가 답을 바꾸지 않는다).
TEST(TextureBlockEac_MatchesExhaustiveSearch) {
    golden::Random random(0x5eed);
    uint16 values[TextureBlockTexels];
    for (uint32 trial = 0; trial < 12; ++trial) {
        // 범위를 섞는다: 좁은 범위(작은 multiplier), 넓은 범위, 전체.
        const uint32 span = trial % 3 == 0 ? 300 : trial % 3 == 1 ? 6000 : 65535;
        const uint32 low = random.Next() % (65536 - span);
        for (uint16& value : values) {
            value = static_cast<uint16>(low + random.Next() % (span + 1));
        }

        CheckMatchesExhaustive(values, 0);
    }

    // 단색(0·중간·최대), 두 값, 0과 최대 양 끝, 최대 근처 몰림, 0 근처 몰림(팔레트 위끝이 0으로
    // 잘림).
    for (const uint16 solid : {uint16{0}, uint16{30000}, uint16{65535}}) {
        for (uint16& value : values) {
            value = solid;
        }

        CheckMatchesExhaustive(values, 0);
    }

    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = i % 2 == 0 ? uint16{1000} : uint16{50000};
    }

    CheckMatchesExhaustive(values, 0);
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = i < 8 ? uint16{0} : uint16{65535};
    }

    CheckMatchesExhaustive(values, 0);
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = static_cast<uint16>(65535 - i * 3);
    }

    CheckMatchesExhaustive(values, 0);
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = static_cast<uint16>(i * 7);
    }

    CheckMatchesExhaustive(values, 0);
    // 가장자리 가중치: 일부 texel만 오차에 넣는다(채운 texel 값은 답에 영향이 없다).
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = static_cast<uint16>(random.Next());
    }

    CheckMatchesExhaustive(values, 0x0033);
    CheckMatchesExhaustive(values, 0x0001);
}

// 채운 texel은 오차에 넣지 않는다: 3 × 2 그림의 블록은 유효 texel 여섯만 본 EncodeEacR11Block과
// 같다.
TEST(TextureBlockEac_EdgeTexelsHaveNoWeight) {
    const uint16 source[6] = {100, 40000, 65535, 0, 20000, 777};
    uint8 image[sizeof(source)];
    for (uint32 i = 0; i < 6; ++i) {
        image[i * 2] = static_cast<uint8>(source[i] & 0xFF);
        image[i * 2 + 1] = static_cast<uint8>(source[i] >> 8);
    }

    TextureBlockImage view;
    view.rgba = Span<const uint8>(image, sizeof(image));
    view.sizeX = 3;
    view.sizeY = 2;
    uint8 out[EacBlock];
    REQUIRE(EncodeTextureBlocks(TextureBlockFormat::EacR11, view, Span<uint8>(out, EacBlock)));
    // NVTT 주기 반복(폭 3: 0,1,2,0, 높이 2: 0,1,0,1)으로 채우고 유효 texel만 켠다.
    uint16 values[TextureBlockTexels];
    constexpr uint32 Columns[4] = {0, 1, 2, 0};
    constexpr uint32 Rows[4] = {0, 1, 0, 1};
    for (uint32 row = 0; row < 4; ++row) {
        for (uint32 column = 0; column < 4; ++column) {
            values[row * 4 + column] = source[Rows[row] * 3 + Columns[column]];
        }
    }

    uint8 expected[EacBlock];
    EncodeEacR11Block(values, 0x0077, expected);
    CHECK(memcmp(out, expected, EacBlock) == 0);
    CheckMatchesExhaustive(values, 0x0077);
}

// 품질: 같은 16 bit 원본에서 EAC R11의 11 bit 오차는 BC4(8 bit 입력·팔레트)보다 크지 않다(묶음 단위
// — 블록 단위 불변식은 아니다).
TEST(TextureBlockEac_QualityNotBelowBc4) {
    SystemAllocator allocator;
    constexpr uint32 Size = 32;
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(CaseNamed("value-noise"), rgba));
    double eacError = 0.0;
    double bc4Error = 0.0;
    for (uint32 by = 0; by < Size; by += 4) {
        for (uint32 bx = 0; bx < Size; bx += 4) {
            uint16 values[TextureBlockTexels];
            uint8 texels[TextureBlockRgbaBytes];
            for (uint32 i = 0; i < TextureBlockTexels; ++i) {
                const uint32 x = bx + i % 4;
                const uint32 y = by + i / 4;
                // 16 bit 원본: R·G 두 byte로 만든다.
                const uint8* texel = rgba.Data() + (usize{y} * Size + x) * Channels;
                values[i] = static_cast<uint16>(texel[0] << 8 | texel[1]);
                texels[i * 4] = static_cast<uint8>((uint32{values[i]} * 255 + 32767) / 65535);
                texels[i * 4 + 1] = 0;
                texels[i * 4 + 2] = 0;
                texels[i * 4 + 3] = 255;
            }

            uint8 eac[EacBlock];
            EncodeEacR11Block(values, 0, eac);
            eacError += static_cast<double>(BlockError(eac, values, 0xFFFF));
            uint8 bc4[EacBlock];
            EncodeTextureBlock(TextureBlockFormat::Bc4, texels, bc4);
            uint8 decoded[TextureBlockRgbaBytes];
            DecodeTextureBlock(TextureBlockFormat::Bc4, bc4, decoded);
            for (uint32 i = 0; i < TextureBlockTexels; ++i) {
                const double delta = Target11(values[i]) - decoded[i * 4] * 2047.0 / 255.0;
                bc4Error += delta * delta;
            }
        }
    }

    printf("  EAC R11 vs BC4 (11-bit SSE): %.0f vs %.0f\n", eacError, bc4Error);
    CHECK(eacError <= bc4Error);
}

// ASTC 왕복 품질: 블록 크기마다 PSNR 하한이고, 큰 블록일수록 낮아진다(크기 대응이 맞음).
TEST(TextureBlockAstc_RoundTripQuality) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("value-noise");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    double previous = PerfectPsnr + 1.0;
    for (uint32 index = 0; index < 5; ++index) {
        const TextureBlockFormat format = AstcFormats[index];
        Array<uint8> blocks(allocator);
        REQUIRE(
            EncodeAstc(format, rgba.Data(), one.sizeX, one.sizeY, TextureBlockOptions{}, blocks));
        Array<uint8> decoded(allocator);
        REQUIRE(DecodeAstc(format, blocks, one.sizeX, one.sizeY, false, decoded));
        double sum = 0.0;
        for (usize i = 0; i < rgba.Size(); ++i) {
            const double delta = static_cast<double>(rgba[i]) - static_cast<double>(decoded[i]);
            sum += delta * delta;
        }

        const double psnr = Psnr(sum, static_cast<double>(rgba.Size()), 255.0);
        printf("  ASTC %ux%u PSNR %.2f dB\n", TextureBlockWidth(format), TextureBlockHeight(format),
               psnr);
        CHECK(psnr >= AstcMinimumPsnr[index]);
        CHECK(psnr <= previous);
        previous = psnr;
    }
}

// 채널 배치: Rgb는 입력 알파를 보지 않고 1로 쓴다, 노멀 LA는 R = G = B = X·A = Y로 푼다.
TEST(TextureBlockAstc_ChannelModes) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("alpha-edge");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    Array<uint8> opaque(allocator);
    REQUIRE(opaque.TryResize(rgba.Size()));
    for (usize i = 0; i < rgba.Size(); ++i) {
        opaque[i] = i % Channels == 3 ? uint8{17} : rgba[i];
    }

    TextureBlockOptions rgb;
    rgb.astcMode = TextureAstcMode::Rgb;
    Array<uint8> first(allocator);
    Array<uint8> second(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc6x6, rgba.Data(), one.sizeX, one.sizeY, rgb, first));
    REQUIRE(
        EncodeAstc(TextureBlockFormat::Astc6x6, opaque.Data(), one.sizeX, one.sizeY, rgb, second));
    CHECK(first.Size() == second.Size() && memcmp(first.Data(), second.Data(), first.Size()) == 0);
    Array<uint8> decoded(allocator);
    REQUIRE(DecodeAstc(TextureBlockFormat::Astc6x6, first, one.sizeX, one.sizeY, false, decoded));
    bool alphaOne = true;
    for (usize i = 3; i < decoded.Size(); i += Channels) {
        alphaOne = alphaOne && decoded[i] == 255;
    }

    CHECK(alphaOne);
    // Rgba는 알파를 지킨다(알파 가장자리 그림 — 출력이 달라야 한다).
    Array<uint8> withAlpha(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc6x6, rgba.Data(), one.sizeX, one.sizeY,
                       TextureBlockOptions{}, withAlpha));
    CHECK(memcmp(first.Data(), withAlpha.Data(), first.Size()) != 0);

    const golden::Case& normal = CaseNamed("normal");
    Array<uint8> normals(allocator);
    REQUIRE(GenerateRgba(normal, normals));
    TextureBlockOptions la;
    la.astcMode = TextureAstcMode::NormalLa;
    Array<uint8> blocks(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc6x6, normals.Data(), normal.sizeX, normal.sizeY, la,
                       blocks));
    REQUIRE(DecodeAstc(TextureBlockFormat::Astc6x6, blocks, normal.sizeX, normal.sizeY, false,
                       decoded));
    bool luminance = true;
    double xError = 0.0;
    double yError = 0.0;
    for (usize i = 0; i < decoded.Size(); i += Channels) {
        luminance = luminance && decoded[i] == decoded[i + 1] && decoded[i] == decoded[i + 2];
        const double dx = static_cast<double>(decoded[i]) - normals[i];
        const double dy = static_cast<double>(decoded[i + 3]) - normals[i + 1];
        xError += dx * dx;
        yError += dy * dy;
    }

    const double count = static_cast<double>(decoded.Size() / Channels);
    printf("  ASTC normal LA PSNR X %.2f dB, Y %.2f dB\n", Psnr(xError, count, 255.0),
           Psnr(yError, count, 255.0));
    CHECK(luminance);
    CHECK(Psnr(xError, count, 255.0) >= 30.0);
    CHECK(Psnr(yError, count, 255.0) >= 30.0);
}

// sRGB profile은 출력이 다르고(같은 byte를 감마 공간으로 본다), 같은 profile로 풀면 품질 하한을
// 넘는다.
TEST(TextureBlockAstc_SrgbProfile) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("gradient");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    TextureBlockOptions srgb;
    srgb.srgb = true;
    Array<uint8> linear(allocator);
    Array<uint8> gamma(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc8x8, rgba.Data(), one.sizeX, one.sizeY,
                       TextureBlockOptions{}, linear));
    REQUIRE(
        EncodeAstc(TextureBlockFormat::Astc8x8, rgba.Data(), one.sizeX, one.sizeY, srgb, gamma));
    CHECK(memcmp(linear.Data(), gamma.Data(), linear.Size()) != 0);
    Array<uint8> decoded(allocator);
    REQUIRE(DecodeAstc(TextureBlockFormat::Astc8x8, gamma, one.sizeX, one.sizeY, true, decoded));
    double sum = 0.0;
    for (usize i = 0; i < rgba.Size(); ++i) {
        const double delta = static_cast<double>(rgba[i]) - static_cast<double>(decoded[i]);
        sum += delta * delta;
    }

    CHECK(Psnr(sum, static_cast<double>(rgba.Size()), 255.0) >= 30.0);
}

// slice 여럿 = slice마다 따로 인코딩한 결과, pitch 입력(scratch) = 빽빽한 입력, 블록 배수가 아닌
// 크기의 출력 크기, scratch 없는 pitch 입력·출력 크기 오류.
TEST(TextureBlockAstc_SlicesPitchAndErrors) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("value-noise");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    // 30 × 22 두 slice(10×10 블록: 3 × 3 블록 × 16 byte × 2).
    constexpr uint32 X = 30;
    constexpr uint32 Y = 22;
    Array<uint8> slices(allocator);
    REQUIRE(slices.TryResize(usize{X} * Y * Channels * 2));
    for (uint32 slice = 0; slice < 2; ++slice) {
        for (uint32 y = 0; y < Y; ++y) {
            memcpy(slices.Data() + (usize{slice} * Y + y) * X * Channels,
                   rgba.Data() + (usize{y + slice * 5} * one.sizeX) * Channels,
                   usize{X} * Channels);
        }
    }

    Array<uint8> both(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc10x10, slices.Data(), X, Y, TextureBlockOptions{},
                       both, 2));
    CHECK(both.Size() == 3 * 3 * 16 * 2);
    for (uint32 slice = 0; slice < 2; ++slice) {
        Array<uint8> single(allocator);
        REQUIRE(EncodeAstc(TextureBlockFormat::Astc10x10,
                           slices.Data() + usize{slice} * X * Y * Channels, X, Y,
                           TextureBlockOptions{}, single));
        CHECK(memcmp(both.Data() + single.Size() * slice, single.Data(), single.Size()) == 0);
    }

    // pitch: 행마다 12 byte 여백, slice 사이 64 byte 여백.
    constexpr usize Pitch = usize{X} * Channels + 12;
    constexpr usize Plane = Pitch * Y + 64;
    Array<uint8> padded(allocator);
    REQUIRE(padded.TryResize(Plane * 2));
    for (uint32 slice = 0; slice < 2; ++slice) {
        for (uint32 y = 0; y < Y; ++y) {
            memcpy(padded.Data() + Plane * slice + Pitch * y,
                   slices.Data() + (usize{slice} * Y + y) * X * Channels, usize{X} * Channels);
        }
    }

    TextureBlockOptions scratch;
    scratch.scratch = &allocator;
    Array<uint8> pitched(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc10x10, padded.Data(), X, Y, scratch, pitched, 2,
                       Pitch, Plane));
    CHECK(memcmp(pitched.Data(), both.Data(), both.Size()) == 0);
    // scratch가 없으면 행 여백 입력은 거부한다(slice 여백만은 받는다).
    CHECK(!EncodeAstc(TextureBlockFormat::Astc10x10, padded.Data(), X, Y, TextureBlockOptions{},
                      pitched, 2, Pitch, Plane));
    Array<uint8> slicePadded(allocator);
    REQUIRE(slicePadded.TryResize((usize{X} * Y * Channels + 64) * 2));
    for (uint32 slice = 0; slice < 2; ++slice) {
        memcpy(slicePadded.Data() + (usize{X} * Y * Channels + 64) * slice,
               slices.Data() + usize{slice} * X * Y * Channels, usize{X} * Y * Channels);
    }

    REQUIRE(EncodeAstc(TextureBlockFormat::Astc10x10, slicePadded.Data(), X, Y,
                       TextureBlockOptions{}, pitched, 2, 0, usize{X} * Y * Channels + 64));
    CHECK(memcmp(pitched.Data(), both.Data(), both.Size()) == 0);

    // 출력 크기 오류.
    TextureBlockImage image;
    image.rgba = slices.View();
    image.sizeX = X;
    image.sizeY = Y;
    uint8 small[16];
    CHECK(
        EncodeTextureBlocks(TextureBlockFormat::Astc10x10, image, Span<uint8>(small, 16)).Error() ==
        ErrorCode::InvalidArgument);
    // 디코드 인자 오류: ASTC가 아닌 형식, 크기가 맞지 않는 출력.
    Array<uint8> decoded(allocator);
    REQUIRE(decoded.TryResize(usize{X} * Y * Channels));
    CHECK(DecodeAstcBlocks(TextureBlockFormat::Bc7, both.View(), X, Y, 1, false, decoded.View())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(
        DecodeAstcBlocks(TextureBlockFormat::Astc10x10, both.View(), X, Y, 1, false, decoded.View())
            .Error() == ErrorCode::InvalidArgument);
}

// 호출자의 FTZ·DAZ·반올림 설정과 무관하다(결정적 부동소수 환경 안에서 압축한다).
TEST(TextureBlockAstc_FloatingPointEnvironment) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("radial");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    Array<uint8> plain(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc6x6, rgba.Data(), one.sizeX, one.sizeY,
                       TextureBlockOptions{}, plain));
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit() |
                                                platform::detail::RoundTowardZeroBits());
    Array<uint8> disturbed(allocator);
    const bool encoded = EncodeAstc(TextureBlockFormat::Astc6x6, rgba.Data(), one.sizeX, one.sizeY,
                                    TextureBlockOptions{}, disturbed);
    platform::detail::WriteFloatingPointControl(original);
    REQUIRE(encoded);
    CHECK(memcmp(plain.Data(), disturbed.Data(), plain.Size()) == 0);
}

// 동시 실행: 블록 크기가 다른 두 스레드가 같이 인코딩해도 혼자 인코딩한 결과와 같다(Rocks.patch가
// 없앤 static preset 포인터·공유 각 표 — 12.17.8 2). Join이 끝을 정하므로 시간에 기대지 않는다.
TEST(TextureBlockAstc_ConcurrentContexts) {
    SystemAllocator allocator;
    const golden::Case& one = CaseNamed("value-noise");
    Array<uint8> rgba(allocator);
    REQUIRE(GenerateRgba(one, rgba));
    Array<uint8> small(allocator);
    Array<uint8> large(allocator);
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc4x4, rgba.Data(), one.sizeX, one.sizeY,
                       TextureBlockOptions{}, small));
    REQUIRE(EncodeAstc(TextureBlockFormat::Astc12x12, rgba.Data(), one.sizeX, one.sizeY,
                       TextureBlockOptions{}, large));
    ConcurrentJob first{
        TextureBlockFormat::Astc4x4, rgba.Data(), one.sizeX, one.sizeY, &small, 6, 0};
    ConcurrentJob second{
        TextureBlockFormat::Astc12x12, rgba.Data(), one.sizeX, one.sizeY, &large, 6, 0};
    platform::Thread thread;
    REQUIRE(thread.Start(&EncodeRounds, &first) == platform::ThreadStatus::Success);
    EncodeRounds(&second);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(first.mismatches == 0);
    CHECK(second.mismatches == 0);
}

// 출력 해시 golden: 시험 그림 셋의 ASTC 다섯 크기(RGBA)·6×6 노멀 LA·4×4 sRGB와 EAC R11(R·G 16
// bit)의 FNV-1a 64를 이어 해시한다. 불변성 빌드라 x64와 Arm64가 같은 값이어야 한다(Arm64는 Mac
// 확인).
TEST(TextureBlockEacAstc_OutputHashGolden) {
    SystemAllocator allocator;
    unsigned long long hash = golden::FnvOffset;
    for (const char* name : {"gradient", "value-noise", "normal"}) {
        const golden::Case& one = CaseNamed(name);
        Array<uint8> rgba(allocator);
        REQUIRE(GenerateRgba(one, rgba));
        Array<uint8> out(allocator);
        for (const TextureBlockFormat format : AstcFormats) {
            REQUIRE(
                EncodeAstc(format, rgba.Data(), one.sizeX, one.sizeY, TextureBlockOptions{}, out));
            hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
        }

        TextureBlockOptions la;
        la.astcMode = TextureAstcMode::NormalLa;
        REQUIRE(
            EncodeAstc(TextureBlockFormat::Astc6x6, rgba.Data(), one.sizeX, one.sizeY, la, out));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
        TextureBlockOptions srgb;
        srgb.srgb = true;
        REQUIRE(
            EncodeAstc(TextureBlockFormat::Astc4x4, rgba.Data(), one.sizeX, one.sizeY, srgb, out));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
        Array<uint8> red(allocator);
        REQUIRE(red.TryResize(usize{one.sizeX} * one.sizeY * 2));
        for (usize i = 0; i < usize{one.sizeX} * one.sizeY; ++i) {
            red[i * 2] = rgba[i * Channels + 1];
            red[i * 2 + 1] = rgba[i * Channels];
        }

        auto bytes = TextureBlockOutputBytes(TextureBlockFormat::EacR11, one.sizeX, one.sizeY, 1);
        REQUIRE(bytes);
        REQUIRE(out.TryResize(static_cast<usize>(bytes.Value())));
        TextureBlockImage image;
        image.rgba = red.View();
        image.sizeX = one.sizeX;
        image.sizeY = one.sizeY;
        REQUIRE(EncodeTextureBlocks(TextureBlockFormat::EacR11, image, out.View()));
        hash = golden::Fnv1a64(out.Data(), out.Size(), hash);
    }

    printf("  ASTC/EAC output hash: %016llx\n", hash);
    CHECK(hash == 0x7f9510dbe3f421cdull);
}

} // namespace texture_block_eac_astc_tests_file
