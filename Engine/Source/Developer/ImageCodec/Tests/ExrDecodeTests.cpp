#include "LiveCountingAllocator.h"
#include "ImageFuzz.h"

#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <ImageCodec/ImageEncode.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// OpenEXR 디코더 시험이다(Docs/Design/AssetSystem 12.10절 A5f — OpenEXRCore, UE ExrImageWrapper와
// 같은 채널 규칙). 파일은 OpenEXRCore 쓰기 API로 메모리에 만든다(압축별·scanline·tile·mip
// level·line order·data window 원점·여러 part). 채널 대응(마지막 글자·뒤에서부터 채움·단일 채널),
// 정밀도(part 0 전체), 손실 판정(사용 채널 기준), HTJ2K·deep·subsampling 거부, NaN·Inf 정리, 손상이
// OutOfMemory가 아닌지, 절단, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::image;
namespace exr_decode_tests_file {

using image_codec_test::LiveCountingAllocator;

namespace {

// ---- 쓰기(ImageCodec EncodeExr — 12.10.1절) ----

using ExrChannelSpec = ExrChannelWrite;
using ExrSpec = ExrWriteImage;

Array<uint8> WriteExr(Allocator& allocator, const ExrSpec& spec) noexcept {
    Array<uint8> file(allocator);
    CHECK(EncodeExr(allocator, spec, file));
    return file;
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Unknown, {}, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool HasWarning(const Decoded& decoded, AssetDiagnosticCode code, uint32 detail) noexcept {
    for (const ImageDecodeWarning& warning : decoded.result.warnings) {
        if (warning.code == code && warning.detail == detail) {
            return true;
        }
    }

    return false;
}

uint16 Half(const Decoded& decoded, usize element) noexcept {
    const uint8* at = decoded.result.image.pixels.Data() + element * 2;
    return static_cast<uint16>(at[0] | (at[1] << 8));
}

float Single(const Decoded& decoded, usize element) noexcept {
    const uint8* at = decoded.result.image.pixels.Data() + element * 4;
    return platform::BitCast<float>(static_cast<uint32>(at[0]) | (static_cast<uint32>(at[1]) << 8) |
                                    (static_cast<uint32>(at[2]) << 16) |
                                    (static_cast<uint32>(at[3]) << 24));
}

// half 값 count개를 담는다.
struct Halves {
    explicit Halves(float base, usize count) noexcept : size(count) {
        for (usize index = 0; index < count; ++index) {
            values[index] = Float16::FromFloat(base + static_cast<float>(index)).bits;
        }
    }

    Span<const uint8> Bytes() const noexcept {
        return Span<const uint8>(reinterpret_cast<const uint8*>(values), size * 2);
    }

    uint16 values[64] = {};
    usize size;
};

struct Floats {
    explicit Floats(float base, usize count) noexcept : size(count) {
        for (usize index = 0; index < count; ++index) {
            values[index] = base + static_cast<float>(index) * 0.5f;
        }
    }

    Span<const uint8> Bytes() const noexcept {
        return Span<const uint8>(reinterpret_cast<const uint8*>(values), size * 4);
    }

    float values[64] = {};
    usize size;
};

// 머리의 compression 속성 값을 바꾼다(이름·형식 "compression\0compression\0" + 크기 1 + 값).
void PatchCompression(Array<uint8>& file, uint8 value) noexcept {
    const char key[] = "compression\0compression";
    constexpr usize KeyBytes = sizeof(key);
    for (usize at = 0; at + KeyBytes + 4 < file.Size(); ++at) {
        if (platform::MemoryEquals(file.Data() + at, key, KeyBytes)) {
            file[at + KeyBytes + 4] = value;
            return;
        }
    }

    CHECK(false);
}

} // namespace

// 채널 대응(UE ExrImageWrapper 478–535)과 정밀도(part 0의 모든 채널).
TEST(ExrDecode_ChannelMapping) {
    SystemAllocator allocator;
    const Halves r(1.0f, 4);
    const Halves g(10.0f, 4);
    const Halves b(20.0f, 4);
    const Halves a(30.0f, 4);
    ExrSpec spec;
    spec.width = 2;
    spec.height = 2;
    // 채널 하나면 이름과 무관하게 단일 채널(R16F).
    const ExrChannelSpec single[] = {{"Z", ExrPixelType::Half, r.Bytes()}};
    spec.channels = single;
    Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::R16F &&
          Half(decoded, 3) == r.values[3]);

    // B·G·R: 알파가 없으면 1.
    const ExrChannelSpec rgb[] = {{"B", ExrPixelType::Half, b.Bytes()},
                                  {"G", ExrPixelType::Half, g.Bytes()},
                                  {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = rgb;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA16F);
    CHECK(Half(decoded, 4) == r.values[1] && Half(decoded, 5) == g.values[1] &&
          Half(decoded, 6) == b.values[1] && Half(decoded, 7) == Float16::FromFloat(1.0f).bits);

    // X·Y·Z는 뒤에서부터 채운다 → (Z, Y, X, 1).
    const ExrChannelSpec xyz[] = {{"X", ExrPixelType::Half, r.Bytes()},
                                  {"Y", ExrPixelType::Half, g.Bytes()},
                                  {"Z", ExrPixelType::Half, b.Bytes()}};
    spec.channels = xyz;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == b.values[0] && Half(decoded, 1) == g.values[0] &&
          Half(decoded, 2) == r.values[0]);

    // Y + A → (Y, 0, 0, A).
    const ExrChannelSpec ya[] = {{"Y", ExrPixelType::Half, g.Bytes()},
                                 {"A", ExrPixelType::Half, a.Bytes()}};
    spec.channels = ya;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == g.values[0] && Half(decoded, 1) == 0 &&
          Half(decoded, 2) == 0 && Half(decoded, 3) == a.values[0]);

    // 레이어 이름도 마지막 글자로 대응한다.
    const ExrChannelSpec layer[] = {{"diffuse.B", ExrPixelType::Half, b.Bytes()},
                                    {"diffuse.G", ExrPixelType::Half, g.Bytes()},
                                    {"diffuse.R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = layer;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Half(decoded, 0) == r.values[0] && Half(decoded, 2) == b.values[0]);

    // 다섯 채널: 남는 채널은 경고와 함께 버리고, 그 채널이 FLOAT면 정밀도는 32F다(part 0 전체).
    const Floats q(5.0f, 4);
    const ExrChannelSpec five[] = {{"A", ExrPixelType::Half, a.Bytes()},
                                   {"B", ExrPixelType::Half, b.Bytes()},
                                   {"G", ExrPixelType::Half, g.Bytes()},
                                   {"Q", ExrPixelType::Float, q.Bytes()},
                                   {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = five;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F &&
          HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceChannels, 1));
    CHECK(Single(decoded, 0) == Float16::FromBits(r.values[0]).ToFloat() &&
          Single(decoded, 3) == Float16::FromBits(a.values[0]).ToFloat());

    // UINT 채널은 float로 바꾼다.
    const uint32 uints[4] = {7, 8, 9, 10};
    const ExrChannelSpec uintChannels[] = {
        {"G", ExrPixelType::Uint,
         Span<const uint8>(reinterpret_cast<const uint8*>(uints), sizeof(uints))},
        {"R", ExrPixelType::Uint,
         Span<const uint8>(reinterpret_cast<const uint8*>(uints), sizeof(uints))}};
    spec.channels = uintChannels;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F &&
          Single(decoded, 4) == 8.0f);

    // 한 변 한도는 CapacityExceeded다(라이브러리 머리 검증이 아니라 결과 할당에서 판정한다).
    spec.channels = rgb;
    ImageDecodeLimits tight;
    tight.maxDimension = 1;
    ImageDecodeResult limited(allocator);
    CHECK(!DecodeImage(allocator, WriteExr(allocator, spec).View(), ImageFileFormat::Exr, tight,
                       limited));
    CHECK(limited.failure.code == AssetDiagnosticCode::InvalidSource &&
          limited.failure.error == ErrorCode::CapacityExceeded);

    // subsampling 채널을 쓰면 받지 않는다.
    const Halves small(1.0f, 1);
    const ExrChannelSpec sub[] = {{"G", ExrPixelType::Half, g.Bytes()},
                                  {"R", ExrPixelType::Half, small.Bytes(), 2}};
    spec.channels = sub;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 압축(무손실은 같은 값, 손실 판정은 사용 채널 기준), HTJ2K·deep 거부.
TEST(ExrDecode_Compression) {
    SystemAllocator allocator;
    Halves r(0.0f, 64);
    for (usize index = 0; index < 64; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index % 13) * 0.25f).bits;
    }

    ExrSpec spec;
    spec.width = 8;
    spec.height = 8;
    const ExrChannelSpec halfChannels[] = {{"G", ExrPixelType::Half, r.Bytes()},
                                           {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = halfChannels;
    const Decoded reference = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(reference.ok);
    const ExrCompression lossless[] = {ExrCompression::Rle, ExrCompression::Zips,
                                       ExrCompression::Zip, ExrCompression::Piz,
                                       ExrCompression::Pxr24};
    for (ExrCompression compression : lossless) {
        spec.compression = compression;
        const Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
        CHECK(decoded.ok && platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                                   reference.result.image.pixels.Data(),
                                                   reference.result.image.pixels.Size()));
    }

    // B44·DWA는 half 사용 채널이 있으면 손실이다.
    const ExrCompression halfLossy[] = {ExrCompression::B44, ExrCompression::B44a,
                                        ExrCompression::Dwaa, ExrCompression::Dwab};
    for (ExrCompression compression : halfLossy) {
        spec.compression = compression;
        CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                    AssetDiagnosticCode::LossySource));
    }

    // FLOAT만이면 B44는 받고(그대로 저장한다) PXR24는 손실이다.
    const Floats f(0.5f, 64);
    const ExrChannelSpec floatChannels[] = {{"R", ExrPixelType::Float, f.Bytes()}};
    spec.channels = floatChannels;
    spec.compression = ExrCompression::B44;
    Decoded decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && Single(decoded, 9) == f.values[9]);
    spec.compression = ExrCompression::Pxr24;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::LossySource));
    // 사용하지 않는 채널의 형식은 손실 판정에 들지 않는다: half R·G·B·A + 버리는 FLOAT Q는 PXR24로
    // 받는다.
    const ExrChannelSpec withUnused[] = {{"A", ExrPixelType::Half, r.Bytes()},
                                         {"B", ExrPixelType::Half, r.Bytes()},
                                         {"G", ExrPixelType::Half, r.Bytes()},
                                         {"Q", ExrPixelType::Float, f.Bytes()},
                                         {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = withUnused;
    decoded = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(decoded.ok && HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceChannels, 1));

    // HTJ2K: 디코딩 전에 UnsupportedSourceFormat(InvalidSource가 아니다).
    spec.channels = halfChannels;
    spec.compression = ExrCompression::None;
    Array<uint8> htj2k = WriteExr(allocator, spec);
    PatchCompression(htj2k, static_cast<uint8>(ExrCompression::Htj2k256));
    CHECK(Fails(Decode(allocator, htj2k.View()), AssetDiagnosticCode::UnsupportedSourceFormat));
    PatchCompression(htj2k, static_cast<uint8>(ExrCompression::Htj2k32));
    CHECK(Fails(Decode(allocator, htj2k.View()), AssetDiagnosticCode::UnsupportedSourceFormat));

    // deep scanline은 받지 않는다.
    spec.storage = ExrStorage::DeepScanline;
    CHECK(Fails(Decode(allocator, WriteExr(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 배치: tile(부분 tile·RANDOM_Y·mip level), DECREASING_Y, data window 원점, 여러 part, NaN·Inf.
TEST(ExrDecode_Layout) {
    SystemAllocator allocator;
    Halves r(0.0f, 0);
    r.size = 60;
    for (usize index = 0; index < 60; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index)).bits;
    }

    ExrSpec spec;
    spec.width = 10;
    spec.height = 6;
    const ExrChannelSpec channels[] = {{"G", ExrPixelType::Half, r.Bytes()},
                                       {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = channels;
    const Decoded reference = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(reference.ok && Half(reference, (9 + 5 * 10) * 4) == r.values[59]);
    const auto same = [&reference](const Decoded& decoded) noexcept {
        return decoded.ok &&
               decoded.result.image.pixels.Size() == reference.result.image.pixels.Size() &&
               platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                      reference.result.image.pixels.Data(),
                                      reference.result.image.pixels.Size());
    };

    spec.lineOrder = ExrLineOrder::DecreasingY;
    spec.compression = ExrCompression::Zip;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = ExrLineOrder::IncreasingY;
    spec.minX = -3;
    spec.minY = 5;
    const Decoded shifted = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(shifted) && shifted.result.image.width == 10 && shifted.result.image.height == 6);
    spec.minX = 0;
    spec.minY = 0;

    // tile 4×4(가장자리 부분 tile), RANDOM_Y, mip level(버린 level 수 경고).
    spec.storage = ExrStorage::Tiled;
    spec.tileWidth = 4;
    spec.tileHeight = 4;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = ExrLineOrder::RandomY;
    CHECK(same(Decode(allocator, WriteExr(allocator, spec).View())));
    spec.lineOrder = ExrLineOrder::IncreasingY;
    spec.levels = ExrLevelMode::MipmapLevels;
    const Decoded mips = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(mips) && HasWarning(mips, AssetDiagnosticCode::IgnoredSourceMips, 3));
    spec.levels = ExrLevelMode::OneLevel;
    spec.storage = ExrStorage::Scanline;

    // 여러 part: part 0만, IgnoredSourceImages.
    spec.parts = 2;
    const Decoded parts = Decode(allocator, WriteExr(allocator, spec).View());
    CHECK(same(parts) && HasWarning(parts, AssetDiagnosticCode::IgnoredSourceImages, 1));
    spec.parts = 1;

    // half NaN → 0, +Inf → 65504.
    Halves special(0.0f, 0);
    special.size = 2;
    special.values[0] = 0x7E00;
    special.values[1] = 0x7C00;
    ExrSpec tiny;
    tiny.width = 2;
    const ExrChannelSpec specialChannel[] = {{"R", ExrPixelType::Half, special.Bytes()}};
    tiny.channels = specialChannel;
    const Decoded cleaned = Decode(allocator, WriteExr(allocator, tiny).View());
    CHECK(cleaned.ok && Half(cleaned, 0) == 0 && Half(cleaned, 1) == 0x7BFF);
}

// 손상(chunk 내용이 깨지면 OutOfMemory가 아니라 InvalidSource), 절단, 할당 실패.
TEST(ExrDecode_DamageAndAllocation) {
    SystemAllocator allocator;
    Halves r(0.0f, 0);
    r.size = 64;
    for (usize index = 0; index < 64; ++index) {
        r.values[index] = Float16::FromFloat(static_cast<float>(index % 7)).bits;
    }

    ExrSpec spec;
    spec.width = 8;
    spec.height = 8;
    const ExrChannelSpec channels[] = {{"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = channels;
    const ExrCompression compressions[] = {ExrCompression::Zip, ExrCompression::Piz};
    for (ExrCompression compression : compressions) {
        spec.compression = compression;
        Array<uint8> file = WriteExr(allocator, spec);
        CHECK(Decode(allocator, file.View()).ok);
        // 마지막 chunk 내용의 뒤쪽 byte를 뒤집는다(deflate 넘침·Huffman 손상 경로).
        for (usize back = 1; back <= 8 && back < file.Size(); ++back) {
            file[file.Size() - back] = static_cast<uint8>(file[file.Size() - back] ^ 0xA5);
        }

        const Decoded damaged = Decode(allocator, file.View());
        CHECK(damaged.ok || damaged.result.failure.code == AssetDiagnosticCode::InvalidSource);
        CHECK(!(!damaged.ok && damaged.result.failure.error == ErrorCode::OutOfMemory));
    }

    spec.compression = ExrCompression::Zip;
    const Array<uint8> file = WriteExr(allocator, spec);
    for (usize length = 0; length < file.Size(); ++length) {
        CHECK(!Decode(allocator, file.View().Slice(0, length)).ok);
    }

    const Decoded reference = Decode(allocator, file.View());
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ImageDecodeResult result(failing);
        auto decoded = DecodeImage(failing, file.View(), ImageFileFormat::Exr, {}, result);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(decoded);
            break;
        }

        if (decoded) {
            CHECK(platform::MemoryEquals(result.image.pixels.Data(),
                                         reference.result.image.pixels.Data(),
                                         reference.result.image.pixels.Size()));
            continue;
        }

        CHECK(decoded.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);
}

// 쓰기 할당 실패 전수(12.10.1절 — 14.1.119 후속 1번): EncodeExr의 n번째 할당(OpenEXRCore 묶음, 출력
// 파일, 0 버퍼 — 출력 Array도 같은 Allocator)이 실패하면 OutOfMemory이고 out은 비며 받은 블록이
// 모두 돌아온다. 실패가 없으면 기준과 같은 byte다. 구성: Zip scanline, Piz tile mip level, part 둘
// RLE.
TEST(ExrEncode_AllocationFailures) {
    SystemAllocator allocator;
    constexpr int32 Size = 16;
    uint16 halves[Size * Size] = {};
    for (int32 index = 0; index < Size * Size; ++index) {
        halves[index] = Float16::FromFloat(static_cast<float>(index % 11) * 0.25f).bits;
    }

    const Span<const uint8> bytes(reinterpret_cast<const uint8*>(halves), sizeof(halves));
    const ExrChannelWrite channels[] = {{"R", ExrPixelType::Half, bytes},
                                        {"G", ExrPixelType::Half, bytes}};
    ExrWriteImage images[3];
    for (ExrWriteImage& image : images) {
        image.width = Size;
        image.height = Size;
        image.channels = channels;
    }

    images[0].compression = ExrCompression::Zip;
    images[1].compression = ExrCompression::Piz;
    images[1].storage = ExrStorage::Tiled;
    images[1].tileWidth = 8;
    images[1].tileHeight = 8;
    images[1].levels = ExrLevelMode::MipmapLevels;
    images[2].compression = ExrCompression::Rle;
    images[2].parts = 2;
    for (const ExrWriteImage& image : images) {
        Array<uint8> reference(allocator);
        REQUIRE(EncodeExr(allocator, image, reference));
        uint32 failures = 0;
        uint64 firstLeak = 0;
        for (uint64 attempt = 1;; ++attempt) {
            LiveCountingAllocator counted(allocator);
            test::NFailAllocator failing(counted);
            bool exhausted = false;
            bool ok = false;
            bool expected = true;
            {
                Array<uint8> out(failing);
                failing.FailOnAttempt(attempt);
                const Result<void> encoded = EncodeExr(failing, image, out);
                exhausted = failing.Attempts() < attempt;
                failing.DisableFailure();
                ok = encoded.IsOk();
                expected =
                    ok ? out.Size() == reference.Size() &&
                             platform::MemoryEquals(out.Data(), reference.Data(), reference.Size())
                       : encoded.Error() == ErrorCode::OutOfMemory && out.IsEmpty();
            }

            if (counted.Live() != 0 && firstLeak == 0) {
                firstLeak = attempt;
            }

            CHECK(expected);
            if (!ok) {
                ++failures;
            }

            if (exhausted) {
                CHECK(ok);
                break;
            }
        }

        CHECK(firstLeak == 0);
        CHECK(failures > 0);
    }
}

using image_codec_test::FuzzDecode;
using image_codec_test::FuzzStats;

// 고정 seed 변조 fuzz(Docs/Design/AssetSystem 14.1.119 후속 3번 — ImageFuzz.h): OpenEXR(무압축 RGB
// half, Zip RGBA, Piz tile, RLE 단일 float) 씨앗. 변조한 파일은 결과가 서로 맞는 성공이거나 다섯
// 분류의 실패이고, 할당 블록을 다 돌려준다.
TEST(ExrDecode_FixedSeedMutationFuzz) {
    SystemAllocator allocator;
    const Halves r(1.0f, 16);
    const Halves g(10.0f, 16);
    const Halves b(20.0f, 16);
    const Halves a(30.0f, 16);
    ExrSpec spec;
    spec.width = 4;
    spec.height = 4;
    const ExrChannelSpec rgb[] = {{"B", ExrPixelType::Half, b.Bytes()},
                                  {"G", ExrPixelType::Half, g.Bytes()},
                                  {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = rgb;
    const Array<uint8> plain = WriteExr(allocator, spec);
    const ExrChannelSpec rgba[] = {{"A", ExrPixelType::Half, a.Bytes()},
                                   {"B", ExrPixelType::Half, b.Bytes()},
                                   {"G", ExrPixelType::Half, g.Bytes()},
                                   {"R", ExrPixelType::Half, r.Bytes()}};
    spec.channels = rgba;
    spec.compression = ExrCompression::Zip;
    const Array<uint8> zip = WriteExr(allocator, spec);
    spec.compression = ExrCompression::Piz;
    spec.storage = ExrStorage::Tiled;
    spec.tileWidth = 2;
    spec.tileHeight = 2;
    const Array<uint8> piz = WriteExr(allocator, spec);
    ExrSpec single;
    single.width = 4;
    single.height = 2;
    single.compression = ExrCompression::Rle;
    const ExrChannelSpec y[] = {{"Y", ExrPixelType::Half, Span<const uint8>(r.Bytes().Data(), 16)}};
    single.channels = y;
    const Array<uint8> rle = WriteExr(allocator, single);
    const Span<const uint8> seeds[] = {plain.View(), zip.View(), piz.View(), rle.View()};
    for (const Span<const uint8>& seed : seeds) {
        REQUIRE(!seed.IsEmpty());
    }

    FuzzStats stats;
    CHECK(FuzzDecode(allocator, seeds, ImageFileFormat::Exr, 0x5EED0E52ull, 3000, stats));
    CHECK(stats.Clean() && stats.rejected > 0);
}

} // namespace exr_decode_tests_file
