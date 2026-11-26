#pragma once

#include "LiveCountingAllocator.h"

#include <Core/Containers/Array.h>
#include <Core/Encoding/Crc32.h>
#include <ImageCodec/ImageDecode.h>
#include <Texture/TextureSource.h>

// 이미지 디코더 고정 seed 변조 fuzz 도구다(Docs/Design/AssetSystem 14.1.119 후속 3번): 올바른 씨앗
// 파일에 변조(자르기·byte 덧붙이기·byte 1–4개 xor)를 걸어 디코딩하고, 성공이면 결과가 서로 맞는지,
// 실패면 ImageDecodeFailure의 다섯 분류인지, 시도마다 할당 블록을 다 돌려주는지 본다.
namespace rocks::image_codec_test {

using image::DecodedImage;
using image::ImageDecodeFailure;
using image::ImageDecodeLimits;
using image::ImageDecodeResult;
using image::ImageFileFormat;

// 시험용 디코딩 한도와 블록 상한이다(변조한 머리가 큰 할당을 부르지 않게 — 실행은 메모리 상한
// 안에서 한다).
inline constexpr uint32 FuzzMaxDimension = 4096;
inline constexpr uint64 FuzzMaxPixelBytes = uint64{64} << 20;
inline constexpr usize FuzzMaxBlockBytes = usize{256} << 20;
// 변조 하나가 바꾸는 byte 수의 최댓값이다.
inline constexpr uint64 FuzzMaxFlips = 4;
// 변조 모양의 수(0 자르기, 1 덧붙이기, 나머지 xor)다.
inline constexpr uint64 FuzzShapes = 8;
// PNG 서명과 chunk 머리(길이·종류)·CRC의 byte 수다.
inline constexpr usize PngSignatureBytes = 8;
inline constexpr usize PngChunkHeadBytes = 8;
inline constexpr usize PngCrcBytes = 4;

// 결정적 의사 난수다(xorshift64).
struct FuzzRandom {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// 블록 하나가 FuzzMaxBlockBytes를 넘는 요청을 OutOfMemory로 거부한다(refused로 센다).
class CappedAllocator final : public Allocator {
public:
    explicit CappedAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (bytes > FuzzMaxBlockBytes) {
            ++refused;
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        inner_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        inner_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        inner_.MarkThreadActive();
    }

    uint64 refused = 0;

private:
    Allocator& inner_;
};

// fuzz 결과다. 위반이 있으면 처음 것의 씨앗 번호·시도 번호를 남긴다.
struct FuzzStats {
    static constexpr uint32 NoViolation = ~uint32{0};

    uint32 accepted = 0;
    uint32 rejected = 0;
    uint32 badSeed = NoViolation;
    uint32 badIteration = NoViolation;

    bool Clean() const noexcept {
        return badSeed == NoViolation;
    }
};

inline uint32 ReadBigEndian32(const uint8* bytes) noexcept {
    return (uint32{bytes[0]} << 24) | (uint32{bytes[1]} << 16) | (uint32{bytes[2]} << 8) |
           uint32{bytes[3]};
}

// PNG chunk CRC를 다시 맞춘다(서명 뒤부터, 길이가 파일을 넘는 chunk에서 멈춘다).
inline void FixPngCrcs(Array<uint8>& file) noexcept {
    usize offset = PngSignatureBytes;
    while (offset + PngChunkHeadBytes + PngCrcBytes <= file.Size()) {
        const uint64 length = ReadBigEndian32(file.Data() + offset);
        const uint64 end = uint64{offset} + PngChunkHeadBytes + length;
        if (end + PngCrcBytes > file.Size()) {
            return;
        }

        const usize typeAt = offset + PngChunkHeadBytes - 4;
        const uint32 crc =
            Crc32(Span<const uint8>(file.Data() + typeAt, static_cast<usize>(end) - typeAt));
        uint8* out = file.Data() + static_cast<usize>(end);
        out[0] = static_cast<uint8>(crc >> 24);
        out[1] = static_cast<uint8>(crc >> 16);
        out[2] = static_cast<uint8>(crc >> 8);
        out[3] = static_cast<uint8>(crc);
        offset = static_cast<usize>(end) + PngCrcBytes;
    }
}

// 실패가 ImageDecodeFailure의 다섯 분류 중 하나인지다(OutOfMemory는 상한 거부가 있었을 때만).
inline bool AcceptableFailure(const ImageDecodeFailure& failure, bool capped) noexcept {
    switch (failure.code) {
    case AssetDiagnosticCode::InvalidSource:
        return failure.error == ErrorCode::CorruptData ||
               failure.error == ErrorCode::CapacityExceeded;
    case AssetDiagnosticCode::UnsupportedSourceFormat:
    case AssetDiagnosticCode::LossySource:
        return failure.error == ErrorCode::Unsupported;
    case AssetDiagnosticCode::None:
        return failure.error == ErrorCode::OutOfMemory && capped;
    default:
        return false;
    }
}

// 성공한 결과가 서로 맞는지다(유효 형식, 한도 안 크기, pixels byte = 폭 × 높이 × slice × texel).
inline bool ConsistentImage(const DecodedImage& image, const ImageDecodeLimits& limits) noexcept {
    if (image.format == TextureSourceFormat::Invalid || image.width == 0 || image.height == 0 ||
        image.numSlices == 0 || image.width > limits.maxDimension ||
        image.height > limits.maxDimension || image.numSlices > limits.maxSlices) {
        return false;
    }

    const uint64 bytes = uint64{image.width} * image.height * image.numSlices *
                         TextureSourceTexelBytes(image.format);
    return bytes <= limits.maxPixelBytes && image.pixels.Size() == bytes;
}

// seeds 각각을 그대로 한 번(성공해야 한다), 그다음 iterations번 변조해 format으로 디코딩한다(PNG는
// 짝수 시도에서 chunk CRC를 다시 맞춘다). 위반이 없으면 참이다.
inline bool FuzzDecode(Allocator& base, Span<const Span<const uint8>> seeds, ImageFileFormat format,
                       uint64 seed, uint32 iterations, FuzzStats& stats,
                       StringView pathHint = {}) noexcept {
    ImageDecodeLimits limits;
    limits.maxDimension = FuzzMaxDimension;
    limits.maxPixelBytes = FuzzMaxPixelBytes;
    FuzzRandom random{seed};
    const auto violation = [&](usize seedIndex, uint32 iteration) noexcept {
        stats.badSeed = static_cast<uint32>(seedIndex);
        stats.badIteration = iteration;
        return false;
    };

    for (usize seedIndex = 0; seedIndex < seeds.Size(); ++seedIndex) {
        const Span<const uint8> valid = seeds[seedIndex];
        for (uint32 iteration = 0; iteration <= iterations; ++iteration) {
            Array<uint8> input(base);
            if (!input.TryAppend(valid)) {
                return violation(seedIndex, iteration);
            }

            // 시도 0은 변조 없는 씨앗이다.
            if (iteration != 0) {
                const uint64 shape = random.Next() % FuzzShapes;
                if (shape == 0 || input.IsEmpty()) {
                    (void)input.TryResize(
                        input.IsEmpty() ? 0 : static_cast<usize>(random.Next() % input.Size()));
                } else if (shape == 1) {
                    if (!input.TryPushBack(static_cast<uint8>(random.Next()))) {
                        return violation(seedIndex, iteration);
                    }
                } else {
                    const uint64 flips = 1 + random.Next() % FuzzMaxFlips;
                    for (uint64 flip = 0; flip < flips; ++flip) {
                        input[static_cast<usize>(random.Next() % input.Size())] ^=
                            static_cast<uint8>(1 + random.Next() % 255);
                    }
                }

                if (format == ImageFileFormat::Png && iteration % 2 == 0) {
                    FixPngCrcs(input);
                }
            }

            CappedAllocator capped(base);
            LiveCountingAllocator live(capped);
            {
                ImageDecodeResult result(live);
                auto done = DecodeImage(live, input.View(), format, limits, result, pathHint);
                if (done) {
                    if (!ConsistentImage(result.image, limits)) {
                        return violation(seedIndex, iteration);
                    }

                    ++stats.accepted;
                } else {
                    if (iteration == 0 || done.Error() != result.failure.error ||
                        !AcceptableFailure(result.failure, capped.refused != 0)) {
                        return violation(seedIndex, iteration);
                    }

                    ++stats.rejected;
                }
            }

            if (live.Live() != 0) {
                return violation(seedIndex, iteration);
            }
        }
    }

    // 시도마다 성공 또는 실패 하나로 셌다(세지 않고 빠져나간 길이 없다).
    return uint64{stats.accepted} + stats.rejected == uint64{seeds.Size()} * (iterations + 1);
}

} // namespace rocks::image_codec_test
