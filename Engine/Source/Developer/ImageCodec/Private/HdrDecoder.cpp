#include "ImageCodecInternal.h"

// Radiance HDR 디코더다(Docs/Design/AssetSystem 12.10절 — UE HdrImageWrapper.cpp와 같은 규칙): 머리
// 줄(256 byte 이하, `\n`으로 끝, NUL 금지), 첫 줄 `#?RADIANCE`·`#?RGBE`,
// FORMAT=32-bit_rle_rgbe(없으면 그 형식으로 본다 — UE는 실패, Rocks가 상위집합), 해상도 줄 `-Y h +X
// w`, 새 RLE·평·옛 RLE 행. RGBE는 UE FColor::FromRGBE와 같게 RGBA32F로 푼다(정밀도 손실 없음).
namespace rocks::image::detail {
namespace {

constexpr usize HdrMaxLineBytes = 256;
constexpr usize HdrMinFileBytes = 11;
constexpr uint32 HdrNewRleMinWidth = 8;
constexpr uint32 HdrNewRleMaxWidth = 0x7fff;
constexpr uint32 HdrRgbeBytes = 4;
constexpr uint32 HdrRunFlag = 128;
constexpr int32 HdrMaxShift = 32;
constexpr int32 HdrExponentBias = 136;
constexpr uint32 HdrChannels = 4;

// 머리 줄 하나다(끝의 `\n`은 소비하고 담지 않는다).
struct HdrLine {
    const uint8* data = nullptr;
    usize size = 0;

    bool Equals(const char* text) const noexcept {
        usize length = 0;
        while (text[length] != '\0') {
            ++length;
        }

        if (length != size) {
            return false;
        }

        for (usize index = 0; index < size; ++index) {
            if (data[index] != static_cast<uint8>(text[index])) {
                return false;
            }
        }

        return true;
    }
};

// UE GetHeaderLine과 같다: `\n`까지(CR은 문자), NUL·256 byte 초과·입력 끝은 실패다.
bool HdrReadLine(Span<const uint8> file, usize& at, HdrLine& line) noexcept {
    const usize start = at;
    for (usize count = 0; count < HdrMaxLineBytes; ++count) {
        if (at >= file.Size() || file[at] == 0) {
            return false;
        }

        if (file[at] == '\n') {
            line.data = file.Data() + start;
            line.size = at - start;
            ++at;
            return true;
        }

        ++at;
    }

    return false;
}

// line이 prefix로 시작하면 cursor를 그 뒤로 옮긴다.
bool HdrMatch(const HdrLine& line, usize& cursor, const char* prefix) noexcept {
    usize at = cursor;
    for (usize index = 0; prefix[index] != '\0'; ++index, ++at) {
        if (at >= line.size || line.data[at] != static_cast<uint8>(prefix[index])) {
            return false;
        }
    }

    cursor = at;
    return true;
}

// 양의 10진 정수다(UE ParsePositiveInt — 부호 없음, int 범위).
bool HdrParsePositive(const HdrLine& line, usize& cursor, uint32& value) noexcept {
    if (cursor >= line.size || line.data[cursor] < '0' || line.data[cursor] > '9') {
        return false;
    }

    uint64 parsed = 0;
    while (cursor < line.size && line.data[cursor] >= '0' && line.data[cursor] <= '9') {
        parsed = parsed * 10 + static_cast<uint64>(line.data[cursor] - '0');
        if (parsed > static_cast<uint64>(NumericLimits<int32>::Max())) {
            return false;
        }

        ++cursor;
    }

    value = static_cast<uint32>(parsed);
    return true;
}

// 옛 RLE·평 RGBE다(UE OldDecompressScanline 562–640): (1, 1, 1, n)은 앞 texel 반복이고 연속
// 반복마다 shift가 8씩 늘어난다. 행 첫머리 반복(initialRunAllowed가 false)과 남은 길이를 넘는
// 반복은 실패다.
bool HdrOldScanline(Span<const uint8> file, usize& at, uint8* rgbe, uint32 done, uint32 length,
                    bool initialRunAllowed) noexcept {
    int32 shift = initialRunAllowed ? 0 : HdrMaxShift;
    uint8* outAt = rgbe + done * HdrRgbeBytes;
    uint64 remaining = length;
    while (remaining > 0) {
        if (file.Size() - at < HdrRgbeBytes) {
            return false;
        }

        const uint8* in = file.Data() + at;
        at += HdrRgbeBytes;
        if (in[0] == 1 && in[1] == 1 && in[2] == 1) {
            if (shift >= HdrMaxShift) {
                return false;
            }

            const uint64 count = uint64{in[3]} << shift;
            if (count > remaining) {
                return false;
            }

            remaining -= count;
            for (uint64 n = 0; n < count; ++n) {
                const uint8* previous = outAt - HdrRgbeBytes;
                for (uint32 c = 0; c < HdrRgbeBytes; ++c) {
                    outAt[c] = previous[c];
                }

                outAt += HdrRgbeBytes;
            }

            shift += 8;
        } else {
            for (uint32 c = 0; c < HdrRgbeBytes; ++c) {
                outAt[c] = in[c];
            }

            outAt += HdrRgbeBytes;
            shift = 0;
            --remaining;
        }
    }

    return true;
}

// 행 하나를 RGBE(r, g, b, e 순)로 푼다(UE DecompressScanline 437–548).
bool HdrScanline(Span<const uint8> file, usize& at, uint8* rgbe, uint32 width) noexcept {
    if (width < HdrNewRleMinWidth || width > HdrNewRleMaxWidth) {
        return HdrOldScanline(file, at, rgbe, 0, width, false);
    }

    if (at >= file.Size()) {
        return false;
    }

    if (file[at] != 2) {
        return HdrOldScanline(file, at, rgbe, 0, width, false);
    }

    if (file.Size() - at < HdrRgbeBytes) {
        return false;
    }

    const uint8* head = file.Data() + at;
    at += HdrRgbeBytes;
    if (head[1] != 2 || (head[2] & 0x80u) != 0) {
        // 새 RLE가 아니다 — 머리 4 byte가 첫 texel이고 이어서 반복을 허용한다.
        for (uint32 c = 0; c < HdrRgbeBytes; ++c) {
            rgbe[c] = head[c];
        }

        return HdrOldScanline(file, at, rgbe, 1, width - 1, true);
    }

    // 새 RLE: 채널마다 run(개수 > 128 반복, 그 밖 그대로 — 개수 0은 무동작). (hi << 8 | lo)는
    // 보지 않는다(UE와 같다).
    for (uint32 channel = 0; channel < HdrChannels; ++channel) {
        uint32 index = 0;
        while (index < width) {
            if (at >= file.Size()) {
                return false;
            }

            uint32 count = file[at++];
            if (count > HdrRunFlag) {
                count &= 0x7fu;
                if (at >= file.Size() || width - index < count) {
                    return false;
                }

                const uint8 value = file[at++];
                for (uint32 n = 0; n < count; ++n) {
                    rgbe[(index + n) * HdrRgbeBytes + channel] = value;
                }
            } else {
                if (width - index < count || file.Size() - at < count) {
                    return false;
                }

                for (uint32 n = 0; n < count; ++n) {
                    rgbe[(index + n) * HdrRgbeBytes + channel] = file[at++];
                }
            }

            index += count;
        }
    }

    return true;
}

// 2^exponent(정확한 double)다 — exponent는 −135–119라 double 정규 범위 안이다.
double HdrPowerOfTwo(int32 exponent) noexcept {
    constexpr int32 DoubleExponentBias = 1023;
    constexpr uint32 DoubleMantissaBits = 52;
    return platform::BitCast<double>(static_cast<uint64>(exponent + DoubleExponentBias)
                                     << DoubleMantissaBits);
}

} // namespace

Result<void> DecodeHdr(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    if (file.Size() < HdrMinFileBytes) {
        return Invalid(out, file.Size());
    }

    usize at = 0;
    HdrLine line;
    if (!HdrReadLine(file, at, line) || (!line.Equals("#?RADIANCE") && !line.Equals("#?RGBE"))) {
        return Invalid(out, 0);
    }

    // 빈 줄까지 자유 형식 줄이다. FORMAT만 보고 EXPOSURE 등은 무시한다(UE 54–80).
    for (;;) {
        const usize lineStart = at;
        if (!HdrReadLine(file, at, line)) {
            return Invalid(out, lineStart);
        }

        if (line.size == 0) {
            break;
        }

        usize cursor = 0;
        if (HdrMatch(line, cursor, "FORMAT=")) {
            const HdrLine value{line.data + cursor, line.size - cursor};
            if (value.Equals("32-bit_rle_xyze")) {
                return Unsupported(out, lineStart);
            }

            if (!value.Equals("32-bit_rle_rgbe")) {
                return Unsupported(out, lineStart);
            }
        }
    }

    // 해상도 줄: `-Y h +X w`만, 줄 전체를 소비한다(UE ParseImageSize).
    const usize resolutionStart = at;
    if (!HdrReadLine(file, at, line)) {
        return Invalid(out, resolutionStart);
    }

    usize cursor = 0;
    uint32 width = 0;
    uint32 height = 0;
    if (!HdrMatch(line, cursor, "-Y ")) {
        // 다른 방향(+Y·±X 먼저)은 받지 않는다. 그 밖은 손상이다.
        const bool otherOrientation = line.size >= 2 &&
                                      (line.data[0] == '+' || line.data[0] == '-') &&
                                      (line.data[1] == 'X' || line.data[1] == 'Y');
        return otherOrientation ? Unsupported(out, resolutionStart) : Invalid(out, resolutionStart);
    }

    if (!HdrParsePositive(line, cursor, height)) {
        return Invalid(out, resolutionStart);
    }

    if (!HdrMatch(line, cursor, " +X ")) {
        const bool otherOrientation = HdrMatch(line, cursor, " -X ");
        return otherOrientation ? Unsupported(out, resolutionStart) : Invalid(out, resolutionStart);
    }

    if (!HdrParsePositive(line, cursor, width) || cursor != line.size || width == 0 ||
        height == 0) {
        return Invalid(out, resolutionStart);
    }

    if (auto allocated = AllocateImage(limits, TextureSourceFormat::RGBA32F, width, height, out);
        !allocated) {
        return allocated;
    }

    Array<uint8> row(allocator);
    if (auto sized = row.TryResize(static_cast<usize>(width) * HdrRgbeBytes); !sized) {
        return Resource(out, sized.Error());
    }

    constexpr uint32 FloatBytes = 4;
    uint8* destination = out.image.pixels.Data();
    for (uint32 y = 0; y < height; ++y) {
        const usize rowStart = at;
        if (!HdrScanline(file, at, row.Data(), width)) {
            return Invalid(out, rowStart);
        }

        for (uint32 x = 0; x < width; ++x) {
            const uint8* rgbe = row.Data() + static_cast<usize>(x) * HdrRgbeBytes;
            float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (rgbe[3] != 0) {
                // UE FColor::FromRGBE: (m + 0.5) × 2^(e − 136). 곱은 double에서 정확하고 float로 한
                // 번 반올림한다(float 곱과 같은 값).
                const double scale = HdrPowerOfTwo(static_cast<int32>(rgbe[3]) - HdrExponentBias);
                for (uint32 c = 0; c < 3; ++c) {
                    values[c] = static_cast<float>((static_cast<double>(rgbe[c]) + 0.5) * scale);
                }
            }

            for (uint32 c = 0; c < HdrChannels; ++c) {
                StoreSanitizedFloat(destination + c * FloatBytes, values[c]);
            }

            destination += HdrChannels * FloatBytes;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
