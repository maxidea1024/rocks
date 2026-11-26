#include "ImageFuzz.h"

#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// Radiance HDR 디코더 시험이다(Docs/Design/AssetSystem 12.10절 A5f — UE HdrImageWrapper.cpp와 같은
// 규칙). 머리 줄(첫 줄·FORMAT·무시하는 줄·해상도 줄), 행 부호화 셋(평·옛 RLE·새 RLE), UE와 같은
// 관대함(새 RLE 폭 칸 무시·literal 0 무동작), 손상(행 첫머리 반복·넘침·절단), RGBE →
// float(FColor::FromRGBE).
using namespace rocks;
using namespace rocks::image;
namespace hdr_decode_tests_file {

namespace {

struct Bytes {
    explicit Bytes(Allocator& allocator) noexcept : data(allocator) {
    }

    void U8(uint32 value) noexcept {
        CHECK(data.TryPushBack(static_cast<uint8>(value)));
    }

    void Text(const char* text) noexcept {
        for (; *text != '\0'; ++text) {
            U8(static_cast<uint8>(*text));
        }
    }

    void Rgbe(uint32 r, uint32 g, uint32 b, uint32 e) noexcept {
        U8(r);
        U8(g);
        U8(b);
        U8(e);
    }

    Array<uint8> data;
};

// 머리(첫 줄, FORMAT 줄 — format이 null이면 없음, 빈 줄, 해상도 줄)를 쓴다.
void Header(Bytes& file, uint32 width, uint32 height, const char* format = "32-bit_rle_rgbe",
            const char* first = "#?RADIANCE") noexcept {
    file.Text(first);
    file.Text("\n");
    file.Text("EXPOSURE=2.0\n");
    if (format != nullptr) {
        file.Text("FORMAT=");
        file.Text(format);
        file.Text("\n");
    }

    file.Text("\n-Y ");
    char digits[16] = {};
    const auto number = [&file, &digits](uint32 value) noexcept {
        uint32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);

        while (count > 0) {
            file.U8(static_cast<uint8>(digits[--count]));
        }
    };
    number(height);
    file.Text(" +X ");
    number(width);
    file.Text("\n");
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Hdr, {}, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

float Channel(const Decoded& decoded, uint32 texel, uint32 channel) noexcept {
    const uint8* at = decoded.result.image.pixels.Data() + (texel * 4 + channel) * 4;
    return platform::BitCast<float>(static_cast<uint32>(at[0]) | (static_cast<uint32>(at[1]) << 8) |
                                    (static_cast<uint32>(at[2]) << 16) |
                                    (static_cast<uint32>(at[3]) << 24));
}

// UE FColor::FromRGBE와 같은 기대값이다.
float FromRgbe(uint32 mantissa, uint32 exponent) noexcept {
    if (exponent == 0) {
        return 0.0f;
    }

    double scale = 1.0;
    for (int32 step = static_cast<int32>(exponent) - 136; step > 0; --step) {
        scale *= 2.0;
    }

    for (int32 step = static_cast<int32>(exponent) - 136; step < 0; ++step) {
        scale *= 0.5;
    }

    return static_cast<float>((mantissa + 0.5) * scale);
}

} // namespace

// 평 RGBE와 변환식, 머리 줄 규칙.
TEST(HdrDecode_FlatAndHeader) {
    SystemAllocator allocator;
    Bytes file(allocator);
    Header(file, 3, 1);
    file.Rgbe(128, 64, 0, 129);
    file.Rgbe(255, 255, 255, 136);
    file.Rgbe(10, 20, 30, 0);
    Decoded decoded = Decode(allocator, file.data.View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F &&
          decoded.result.image.width == 3 && decoded.result.image.height == 1);
    if (decoded.ok) {
        CHECK(Channel(decoded, 0, 0) == FromRgbe(128, 129) &&
              Channel(decoded, 0, 1) == FromRgbe(64, 129) &&
              Channel(decoded, 0, 2) == FromRgbe(0, 129) && Channel(decoded, 0, 3) == 1.0f);
        CHECK(Channel(decoded, 1, 0) == 255.5f);
        // e = 0은 (0, 0, 0, 1)이다.
        CHECK(Channel(decoded, 2, 0) == 0.0f && Channel(decoded, 2, 2) == 0.0f &&
              Channel(decoded, 2, 3) == 1.0f);
    }

    // #?RGBE 첫 줄, FORMAT 없음(UE는 실패 — Rocks가 상위집합)도 받는다.
    Bytes noFormat(allocator);
    Header(noFormat, 1, 1, nullptr, "#?RGBE");
    noFormat.Rgbe(1, 2, 3, 128);
    CHECK(Decode(allocator, noFormat.data.View()).ok);

    // xyze·다른 FORMAT은 UnsupportedSourceFormat, 다른 첫 줄·CR 붙은 첫 줄은 손상이다.
    Bytes xyze(allocator);
    Header(xyze, 1, 1, "32-bit_rle_xyze");
    xyze.Rgbe(1, 2, 3, 128);
    CHECK(Fails(Decode(allocator, xyze.data.View()), AssetDiagnosticCode::UnsupportedSourceFormat));
    Bytes crlf(allocator);
    Header(crlf, 1, 1, "32-bit_rle_rgbe", "#?RADIANCE\r");
    crlf.Rgbe(1, 2, 3, 128);
    CHECK(Fails(Decode(allocator, crlf.data.View()), AssetDiagnosticCode::InvalidSource));

    // 해상도 줄: 다른 방향은 UnsupportedSourceFormat, 남는 byte·0 크기는 손상이다.
    const auto resolution = [&](const char* line) noexcept {
        Bytes bytes(allocator);
        bytes.Text("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n");
        bytes.Text(line);
        bytes.Text("\n");
        bytes.Rgbe(1, 2, 3, 128);
        return Decode(allocator, bytes.data.View());
    };
    CHECK(Fails(resolution("+Y 1 +X 1"), AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(resolution("-Y 1 -X 1"), AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(resolution("+X 1 -Y 1"), AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(resolution("-Y 1 +X 1 "), AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(resolution("-Y 0 +X 1"), AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(resolution("-Y 1 +X 99999999999"), AssetDiagnosticCode::InvalidSource));
    CHECK(resolution("-Y 1 +X 1").ok);

    // 한 변 한도는 CapacityExceeded다.
    Bytes wideFile(allocator);
    Header(wideFile, 2, 1);
    wideFile.Rgbe(1, 2, 3, 128);
    wideFile.Rgbe(4, 5, 6, 128);
    ImageDecodeLimits tight;
    tight.maxDimension = 1;
    ImageDecodeResult limited(allocator);
    CHECK(!DecodeImage(allocator, wideFile.data.View(), ImageFileFormat::Hdr, tight, limited));
    CHECK(limited.failure.code == AssetDiagnosticCode::InvalidSource &&
          limited.failure.error == ErrorCode::CapacityExceeded);

    // 머리 줄 256 byte 초과·NUL은 손상이다.
    Bytes longLine(allocator);
    longLine.Text("#?RADIANCE\n");
    for (uint32 index = 0; index < 300; ++index) {
        longLine.Text("x");
    }

    longLine.Text("\n\n-Y 1 +X 1\n");
    longLine.Rgbe(1, 2, 3, 128);
    CHECK(Fails(Decode(allocator, longLine.data.View()), AssetDiagnosticCode::InvalidSource));
    Bytes nul(allocator);
    nul.Text("#?RADIANCE\nA");
    nul.U8(0);
    nul.Text("\n\n-Y 1 +X 1\n");
    nul.Rgbe(1, 2, 3, 128);
    CHECK(Fails(Decode(allocator, nul.data.View()), AssetDiagnosticCode::InvalidSource));
}

// 옛 RLE: (1, 1, 1, n)은 앞 texel 반복이고 연속 반복마다 개수가 256배다. 행 첫머리 반복은 손상이다.
TEST(HdrDecode_OldRle) {
    SystemAllocator allocator;
    // 폭 4(< 8이라 늘 옛 경로): texel A, 반복 3.
    Bytes file(allocator);
    Header(file, 4, 1);
    file.Rgbe(100, 50, 25, 130);
    file.Rgbe(1, 1, 1, 3);
    Decoded decoded = Decode(allocator, file.data.View());
    CHECK(decoded.ok);
    if (decoded.ok) {
        for (uint32 texel = 0; texel < 4; ++texel) {
            CHECK(Channel(decoded, texel, 0) == FromRgbe(100, 130));
        }
    }

    // 연속 반복: 1 + (1 + 1 × 256) = 258 texel.
    Bytes shifted(allocator);
    Header(shifted, 258, 1);
    shifted.Rgbe(7, 7, 7, 128);
    shifted.Rgbe(1, 1, 1, 1);
    shifted.Rgbe(1, 1, 1, 1);
    decoded = Decode(allocator, shifted.data.View());
    CHECK(decoded.ok);
    if (decoded.ok) {
        CHECK(Channel(decoded, 257, 1) == FromRgbe(7, 128));
    }

    // 행 첫머리 반복, 남은 길이를 넘는 반복은 손상이다.
    Bytes leading(allocator);
    Header(leading, 2, 1);
    leading.Rgbe(1, 1, 1, 2);
    CHECK(Fails(Decode(allocator, leading.data.View()), AssetDiagnosticCode::InvalidSource));
    Bytes overrun(allocator);
    Header(overrun, 2, 1);
    overrun.Rgbe(9, 9, 9, 128);
    overrun.Rgbe(1, 1, 1, 2);
    CHECK(Fails(Decode(allocator, overrun.data.View()), AssetDiagnosticCode::InvalidSource));

    // 폭 > 32767도 옛 경로다.
    Bytes wide(allocator);
    Header(wide, 32768, 1);
    wide.Rgbe(3, 3, 3, 128);
    wide.Rgbe(1, 1, 1, 255);
    wide.Rgbe(1, 1, 1, 127);
    wide.Rgbe(1, 1, 1, 0);
    ImageDecodeLimits limits;
    limits.maxDimension = 32768;
    ImageDecodeResult wideResult(allocator);
    CHECK(DecodeImage(allocator, wide.data.View(), ImageFileFormat::Hdr, limits, wideResult));
    CHECK(wideResult.image.width == 32768 && wideResult.image.height == 1);
}

// 새 RLE(폭 8–32767, 머리 2·2·hi·lo): 채널마다 run. 폭 칸은 보지 않고 literal 0은 무동작이다(UE와
// 같다).
TEST(HdrDecode_NewRle) {
    SystemAllocator allocator;
    constexpr uint32 Width = 8;
    const auto rle = [&](uint32 hi, uint32 lo, bool zeroLiteral) noexcept {
        Bytes file(allocator);
        Header(file, Width, 2);
        for (uint32 row = 0; row < 2; ++row) {
            file.U8(2);
            file.U8(2);
            file.U8(hi);
            file.U8(lo);
            // R: run 8 × (10 + row), G: literal 8, B: run 4 + literal 4, E: run 8 × 129.
            file.U8(128 + Width);
            file.U8(10 + row);
            if (zeroLiteral) {
                file.U8(0);
            }

            file.U8(Width);
            for (uint32 x = 0; x < Width; ++x) {
                file.U8(x * 10);
            }

            file.U8(128 + 4);
            file.U8(200);
            file.U8(4);
            for (uint32 x = 0; x < 4; ++x) {
                file.U8(x);
            }

            file.U8(128 + Width);
            file.U8(129);
        }

        return Move(file.data);
    };
    Decoded decoded = Decode(allocator, rle(0, Width, false).View());
    CHECK(decoded.ok);
    if (decoded.ok) {
        CHECK(Channel(decoded, 0, 0) == FromRgbe(10, 129) &&
              Channel(decoded, Width, 0) == FromRgbe(11, 129));
        CHECK(Channel(decoded, 3, 1) == FromRgbe(30, 129) &&
              Channel(decoded, 2, 2) == FromRgbe(200, 129) &&
              Channel(decoded, 6, 2) == FromRgbe(2, 129));
    }

    const Decoded mismatch = Decode(allocator, rle(0, Width + 3, true).View());
    CHECK(mismatch.ok &&
          mismatch.result.image.pixels.Size() == decoded.result.image.pixels.Size() &&
          platform::MemoryEquals(mismatch.result.image.pixels.Data(),
                                 decoded.result.image.pixels.Data(),
                                 decoded.result.image.pixels.Size()));

    // hi의 bit 7이 서 있으면 새 RLE가 아니다 — 머리 4 byte가 첫 texel이다(평 경로).
    Bytes flat(allocator);
    Header(flat, Width, 1);
    flat.Rgbe(2, 2, 0x80, 130);
    flat.Rgbe(1, 1, 1, Width - 1);
    decoded = Decode(allocator, flat.data.View());
    CHECK(decoded.ok);
    if (decoded.ok) {
        CHECK(Channel(decoded, Width - 1, 2) == FromRgbe(0x80, 130));
    }

    // run이 행을 넘치면 손상이다.
    Bytes overflow(allocator);
    Header(overflow, Width, 1);
    overflow.Rgbe(2, 2, 0, Width);
    overflow.U8(128 + Width + 1);
    overflow.U8(1);
    CHECK(Fails(Decode(allocator, overflow.data.View()), AssetDiagnosticCode::InvalidSource));

    // 절단 전수.
    const Array<uint8> full = rle(0, Width, false);
    for (usize length = 0; length < full.Size(); ++length) {
        CHECK(!Decode(allocator, full.View().Slice(0, length)).ok);
    }
}

using image_codec_test::FuzzDecode;
using image_codec_test::FuzzStats;

// 고정 seed 변조 fuzz(Docs/Design/AssetSystem 14.1.119 후속 3번 — ImageFuzz.h): Radiance HDR(평
// RGBE, 옛 RLE, 새 RLE 폭 8) 씨앗. 변조한 파일은 결과가 서로 맞는 성공이거나 다섯 분류의 실패이고,
// 할당 블록을 다 돌려준다.
TEST(HdrDecode_FixedSeedMutationFuzz) {
    SystemAllocator allocator;
    Bytes flat(allocator);
    Header(flat, 3, 2);
    for (uint32 texel = 0; texel < 6; ++texel) {
        flat.Rgbe(10 * texel, 64, 200, 129 + texel);
    }

    Bytes old(allocator);
    Header(old, 4, 1);
    old.Rgbe(100, 50, 25, 130);
    old.Rgbe(1, 1, 1, 3);
    Bytes rle(allocator);
    Header(rle, 8, 1);
    rle.U8(2);
    rle.U8(2);
    rle.U8(0);
    rle.U8(8);
    for (uint32 channel = 0; channel < 4; ++channel) {
        rle.U8(128 + 4);
        rle.U8(10 + channel);
        rle.U8(4);
        for (uint32 x = 0; x < 4; ++x) {
            rle.U8(x * 20 + channel);
        }
    }

    const Span<const uint8> seeds[] = {flat.data.View(), old.data.View(), rle.data.View()};
    FuzzStats stats;
    CHECK(FuzzDecode(allocator, seeds, ImageFileFormat::Hdr, 0x5EED04D2ull, 4000, stats));
    CHECK(stats.Clean() && stats.rejected > 0);
}

} // namespace hdr_decode_tests_file
