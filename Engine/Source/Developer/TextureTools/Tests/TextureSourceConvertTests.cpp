#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureImport.h>
#include <TextureTools/TextureSourceConvert.h>

// 텍스처 원본 형식 변환·재표본 시험이다(Docs/Design/AssetSystem 12.14절 A5j2b): 공통 형식 표 10 ×
// 10, 직접 경로(G8 sRGB → RGBA8 sRGB 256값 복제, 선형 8 → 16비트 v·257, G16 → GA16), 풀기(R32F →
// RGBA32F의 G·B = 0), 좁히는 변환(RGBA32F → G8은 R만, half RTNE·넘치면 Inf), 재표본(균일 영상은
// 균일, 알파 선형), 오류(byte 수·감마·크기 0·작업 메모리), 할당 실패.
using namespace rocks;

namespace texture_source_convert_tests_file {

namespace {

using F = TextureSourceFormat;
using G = TextureSourceGamma;

constexpr F AllFormats[] = {F::G8,     F::GA8,     F::G16,     F::GA16, F::RGBA8,
                            F::RGBA16, F::RGBA16F, F::RGBA32F, F::R16F, F::R32F};

// 12.14절 A5j2b 1번 표(행·열은 AllFormats 순서).
constexpr F CommonTable[10][10] = {
    // G8
    {F::G8, F::GA8, F::G16, F::GA16, F::RGBA8, F::RGBA16, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::RGBA32F},
    // GA8
    {F::GA8, F::GA8, F::GA16, F::GA16, F::RGBA8, F::RGBA16, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::RGBA32F},
    // G16
    {F::G16, F::GA16, F::G16, F::GA16, F::RGBA16, F::RGBA16, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::RGBA32F},
    // GA16
    {F::GA16, F::GA16, F::GA16, F::GA16, F::RGBA16, F::RGBA16, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::RGBA32F},
    // RGBA8
    {F::RGBA8, F::RGBA8, F::RGBA16, F::RGBA16, F::RGBA8, F::RGBA16, F::RGBA32F, F::RGBA32F,
     F::RGBA32F, F::RGBA32F},
    // RGBA16
    {F::RGBA16, F::RGBA16, F::RGBA16, F::RGBA16, F::RGBA16, F::RGBA16, F::RGBA32F, F::RGBA32F,
     F::RGBA32F, F::RGBA32F},
    // RGBA16F
    {F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA16F, F::RGBA32F,
     F::RGBA16F, F::RGBA32F},
    // RGBA32F
    {F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::RGBA32F, F::RGBA32F},
    // R16F
    {F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA16F, F::RGBA32F,
     F::R16F, F::R32F},
    // R32F
    {F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F, F::RGBA32F,
     F::R32F, F::R32F},
};

Result<void> Convert(Allocator& allocator, F from, G fromGamma, uint32 x, uint32 y,
                     Span<const uint8> bytes, F to, G toGamma, uint32 toX, uint32 toY,
                     Array<uint8>& out) noexcept {
    const TextureSourceImage image{from, fromGamma, x, y, 1, bytes};
    return ConvertTextureSource(allocator, image, to, toGamma, toX, toY, out);
}

uint16 Load16(const Array<uint8>& bytes, usize offset) noexcept {
    return static_cast<uint16>(bytes[offset] | (bytes[offset + 1] << 8));
}

float LoadFloat(const Array<uint8>& bytes, usize offset) noexcept {
    uint32 bits = 0;
    for (usize index = 0; index < 4; ++index) {
        bits |= uint32{bytes[offset + index]} << (index * 8);
    }

    return platform::BitCast<float>(bits);
}

void StoreFloat(uint8* target, float value) noexcept {
    const uint32 bits = platform::BitCast<uint32>(value);
    for (usize index = 0; index < 4; ++index) {
        target[index] = static_cast<uint8>(bits >> (index * 8));
    }
}

} // namespace

// 공통 형식 표(12.14절 A5j2b 1번): 모든 쌍과 교환 대칭, Invalid는 Invalid.
TEST(TextureSourceConvert_CommonFormatTable) {
    for (usize row = 0; row < 10; ++row) {
        for (usize column = 0; column < 10; ++column) {
            const F a = AllFormats[row];
            const F b = AllFormats[column];
            if (CommonTextureSourceFormat(a, b) != CommonTable[row][column] ||
                CommonTextureSourceFormat(b, a) != CommonTable[row][column]) {
                test::RecordFailure("common format pair", __FILE__, __LINE__);
            }
        }
    }

    CHECK(CommonTextureSourceFormat(F::Invalid, F::G8) == F::Invalid &&
          CommonTextureSourceFormat(F::RGBA8, F::Invalid) == F::Invalid);
}

// 직접 경로(12.14절 A5j2b 2번): G8 sRGB → RGBA8 sRGB 256값 복제(L, L, L, 255), 선형 G8 → RGBA16의
// v·257, GA8 → RGBA8의 (L, L, L, A), G16 → GA16의 (v, 65535).
TEST(TextureSourceConvert_DirectPaths) {
    SystemAllocator allocator;
    uint8 values[256];
    for (uint32 index = 0; index < 256; ++index) {
        values[index] = static_cast<uint8>(index);
    }

    Array<uint8> out(allocator);
    REQUIRE(Convert(allocator, F::G8, G::Srgb, 16, 16, values, F::RGBA8, G::Srgb, 16, 16, out));
    REQUIRE(out.Size() == 256 * 4);
    bool same = true;
    for (uint32 index = 0; index < 256; ++index) {
        same = same && out[index * 4] == index && out[index * 4 + 1] == index &&
               out[index * 4 + 2] == index && out[index * 4 + 3] == 255;
    }

    CHECK(same);
    out.Clear();
    REQUIRE(
        Convert(allocator, F::G8, G::Linear, 16, 16, values, F::RGBA16, G::Linear, 16, 16, out));
    REQUIRE(out.Size() == 256 * 8);
    same = true;
    for (uint32 index = 0; index < 256; ++index) {
        same = same && Load16(out, index * 8) == index * 257 && Load16(out, index * 8 + 6) == 65535;
    }

    CHECK(same);
    const uint8 ga[] = {7, 9, 200, 1};
    out.Clear();
    REQUIRE(Convert(allocator, F::GA8, G::Srgb, 2, 1, ga, F::RGBA8, G::Srgb, 2, 1, out));
    CHECK(out.Size() == 8 && out[0] == 7 && out[1] == 7 && out[2] == 7 && out[3] == 9 &&
          out[4] == 200 && out[7] == 1);
    const uint8 g16[] = {0x34, 0x12};
    out.Clear();
    REQUIRE(Convert(allocator, F::G16, G::Linear, 1, 1, g16, F::GA16, G::Linear, 1, 1, out));
    CHECK(out.Size() == 4 && Load16(out, 0) == 0x1234 && Load16(out, 2) == 65535);
}

// 풀기와 좁히는 변환(12.14절 A5j2b 2번): R32F → RGBA32F는 (v, 0, 0, 1), RGBA32F → G8 sRGB는
// R만(sRGB 표 — 0·0.5·1 → 0·188·255), RGBA32F → R16F는 RTNE(1 + 2⁻¹¹ → 1)·넘치면 +Inf.
TEST(TextureSourceConvert_DecodeAndNarrowing) {
    SystemAllocator allocator;
    uint8 red[4];
    StoreFloat(red, 0.25f);
    Array<uint8> out(allocator);
    REQUIRE(Convert(allocator, F::R32F, G::Linear, 1, 1, red, F::RGBA32F, G::Linear, 1, 1, out));
    REQUIRE(out.Size() == 16);
    CHECK(LoadFloat(out, 0) == 0.25f && LoadFloat(out, 4) == 0.0f && LoadFloat(out, 8) == 0.0f &&
          LoadFloat(out, 12) == 1.0f);

    // R은 0·0.5·1, G·B는 그와 다른 값(R만 쓰는지 본다).
    uint8 colors[3 * 16];
    const float reds[] = {0.0f, 0.5f, 1.0f};
    for (usize texel = 0; texel < 3; ++texel) {
        StoreFloat(colors + texel * 16, reds[texel]);
        StoreFloat(colors + texel * 16 + 4, 0.9f);
        StoreFloat(colors + texel * 16 + 8, 0.1f);
        StoreFloat(colors + texel * 16 + 12, 1.0f);
    }

    out.Clear();
    REQUIRE(Convert(allocator, F::RGBA32F, G::Linear, 3, 1, colors, F::G8, G::Srgb, 3, 1, out));
    CHECK(out.Size() == 3 && out[0] == 0 && out[1] == 188 && out[2] == 255);

    uint8 halves[2 * 16];
    const float tie = 1.0f + 1.0f / 2048.0f;
    StoreFloat(halves, tie);
    StoreFloat(halves + 16, 65520.0f);
    for (usize texel = 0; texel < 2; ++texel) {
        for (usize channel = 1; channel < 4; ++channel) {
            StoreFloat(halves + texel * 16 + channel * 4, 0.0f);
        }
    }

    out.Clear();
    REQUIRE(Convert(allocator, F::RGBA32F, G::Linear, 2, 1, halves, F::R16F, G::Linear, 2, 1, out));
    constexpr uint16 HalfOne = 0x3C00;
    constexpr uint16 HalfInfinity = 0x7C00;
    CHECK(out.Size() == 4 && Load16(out, 0) == HalfOne && Load16(out, 2) == HalfInfinity);
}

// 재표본(12.14절 A5j2b 3번): 균일한 G8 선형 2×2(100)를 4×4·1×1로 바꿔도 100이고, RGBA8 sRGB의
// 알파는 선형으로 그대로다. 같은 형식·크기면 byte 그대로다.
TEST(TextureSourceConvert_Resample) {
    SystemAllocator allocator;
    const uint8 flat[] = {100, 100, 100, 100};
    Array<uint8> out(allocator);
    REQUIRE(Convert(allocator, F::G8, G::Linear, 2, 2, flat, F::G8, G::Linear, 4, 4, out));
    bool same = out.Size() == 16;
    for (uint8 value : out) {
        same = same && value == 100;
    }

    CHECK(same);
    out.Clear();
    REQUIRE(Convert(allocator, F::G8, G::Linear, 2, 2, flat, F::G8, G::Linear, 1, 1, out));
    CHECK(out.Size() == 1 && out[0] == 100);

    const uint8 rgba[] = {120, 60, 30, 77, 120, 60, 30, 77};
    out.Clear();
    REQUIRE(Convert(allocator, F::RGBA8, G::Srgb, 2, 1, rgba, F::RGBA8, G::Srgb, 4, 1, out));
    same = out.Size() == 16;
    for (usize texel = 0; same && texel < 4; ++texel) {
        same = out[texel * 4] == 120 && out[texel * 4 + 1] == 60 && out[texel * 4 + 2] == 30 &&
               out[texel * 4 + 3] == 77;
    }

    CHECK(same);
    out.Clear();
    REQUIRE(Convert(allocator, F::RGBA8, G::Srgb, 2, 1, rgba, F::RGBA8, G::Srgb, 2, 1, out));
    CHECK(out.Size() == 8 && out[3] == 77 && out[4] == 120);
}

// 오류와 할당: byte 수 불일치·크기 0·대상 Pow22·16비트 대상 sRGB·16비트 원본 sRGB·Invalid 형식은
// InvalidArgument, 작업 메모리 상한 초과는 byte를 읽기 전에 CapacityExceeded, 할당 실패는 그
// 오류다.
TEST(TextureSourceConvert_ErrorsAndAllocations) {
    SystemAllocator allocator;
    const uint8 four[] = {1, 2, 3, 4};
    Array<uint8> out(allocator);
    CHECK(Convert(allocator, F::G8, G::Linear, 2, 1, four, F::G8, G::Linear, 2, 1, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Convert(allocator, F::G8, G::Linear, 2, 2, four, F::G8, G::Linear, 0, 2, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Convert(allocator, F::G8, G::Linear, 2, 2, four, F::G8, G::Pow22, 2, 2, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Convert(allocator, F::G8, G::Linear, 2, 2, four, F::G16, G::Srgb, 2, 2, out).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(
        Convert(allocator, F::G16, G::Srgb, 2, 1, four, F::RGBA16, G::Linear, 2, 1, out).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(Convert(allocator, F::Invalid, G::Linear, 2, 2, four, F::G8, G::Linear, 2, 2, out)
              .Error() == ErrorCode::InvalidArgument);

    // 길이만 큰 span(byte에 닿지 않는다 — 상한 검사가 먼저다).
    constexpr uint32 Huge = 16384;
    const usize hugeBytes = usize{Huge} * Huge * TextureSourceTexelBytes(F::RGBA32F) * 2;
    const TextureSourceImage huge{F::RGBA32F, G::Linear, Huge,
                                  Huge,       2,         Span<const uint8>(four, hugeBytes)};
    CHECK(ConvertTextureSource(allocator, huge, F::RGBA16F, G::Linear, Huge, Huge, out).Error() ==
          ErrorCode::CapacityExceeded);

    // 할당 실패(모든 지점 — 두 단계 변환 + 재표본).
    const uint8 gray[] = {10, 20, 30, 40};
    test::NFailAllocator failing(allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        Array<uint8> target(failing);
        failing.FailOnAttempt(attempt);
        auto converted =
            Convert(failing, F::G8, G::Srgb, 2, 2, gray, F::RGBA32F, G::Linear, 3, 3, target);
        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(converted);
            break;
        }

        CHECK(converted.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 2);
}

} // namespace texture_source_convert_tests_file
