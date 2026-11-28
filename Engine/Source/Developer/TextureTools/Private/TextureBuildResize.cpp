#include "TextureBuildSteps.h"

#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include "TextureImportInternal.h"

#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureBuild.h>
#include <TextureTools/TextureFormatSelect.h>
#include <TextureTools/TextureImport.h>

#include <math.h>

// 텍스처 빌드의 선형화·재표본·패딩이다(Docs/Design/AssetSystem 12.5절 1·3번, 12.2절 1번 — 12.18
// TP-D6로 TextureBuild.cpp에서 나눔).
namespace rocks::texture_build_detail {

namespace build_resize {

// 16비트 정수·16F 상한이다.
constexpr double Unorm8Max = 255.0;
constexpr double Unorm16Max = 65535.0;

// 원본 종류다(12.4.3절 열).
enum class SourceClass : uint8 {
    Unorm8,
    Unorm16,
    Float,
};

SourceClass ClassOf(TextureSourceFormat format) noexcept {
    switch (format) {
    case TextureSourceFormat::G16:
    case TextureSourceFormat::GA16:
    case TextureSourceFormat::RGBA16:
        return SourceClass::Unorm16;
    case TextureSourceFormat::RGBA16F:
    case TextureSourceFormat::RGBA32F:
    case TextureSourceFormat::R16F:
    case TextureSourceFormat::R32F:
        return SourceClass::Float;
    default:
        return SourceClass::Unorm8;
    }
}

uint32 ChannelsOf(TextureSourceFormat format) noexcept {
    switch (format) {
    case TextureSourceFormat::G8:
    case TextureSourceFormat::G16:
    case TextureSourceFormat::R16F:
    case TextureSourceFormat::R32F:
        return 1;
    case TextureSourceFormat::GA8:
    case TextureSourceFormat::GA16:
        return 2;
    default:
        return 4;
    }
}
uint16 LoadLe16(const uint8* data) noexcept {
    return static_cast<uint16>(data[0] | (data[1] << 8));
}

uint32 LoadLe32(const uint8* data) noexcept {
    return static_cast<uint32>(data[0]) | (static_cast<uint32>(data[1]) << 8) |
           (static_cast<uint32>(data[2]) << 16) | (static_cast<uint32>(data[3]) << 24);
}

// ---- 1. 선형화(12.2절 1번) ----

// 색 채널 정수 값 하나를 선형으로 읽는 방식이다.
enum class Decode : uint8 {
    Linear,
    Srgb,
    Gamma22,
};

Decode DecodeFor(const TextureBuildSettings& settings, SourceClass source) noexcept {
    if (source == SourceClass::Float) {
        return Decode::Linear;
    }

    if (settings.encodingOverride == TextureSourceEncoding::Linear) {
        return Decode::Linear;
    }

    if (settings.encodingOverride == TextureSourceEncoding::sRGB) {
        return Decode::Srgb;
    }

    if (source == SourceClass::Unorm16 || !settings.srgb) {
        return Decode::Linear;
    }

    return settings.useLegacyGamma ? Decode::Gamma22 : Decode::Srgb;
}

float DecodeChannel(uint32 value, double maximum, Decode decode) noexcept {
    const double encoded = static_cast<double>(value) / maximum;
    switch (decode) {
    case Decode::Srgb:
        return static_cast<float>(deterministic::SrgbToLinear(encoded));
    case Decode::Gamma22:
        return static_cast<float>(deterministic::Gamma22ToLinear(encoded));
    case Decode::Linear:
        break;
    }

    return static_cast<float>(encoded);
}

// format 원본 byte를 decode 방식으로 푼다(UE CopyImage 원본 → RGBA32F — 12.14절 A5j2b 2번). out은
// width × height × depth × layers다.
Result<void> DecodeFormat(TextureSourceFormat format, Decode decode, uint32 width, uint32 height,
                          uint32 depth, uint32 layers, Span<const uint8> pixels,
                          Level& out) noexcept {
    if (auto made = out.Allocate(width, height, depth, layers); !made) {
        return made;
    }

    const SourceClass source = ClassOf(format);
    const uint32 channels = ChannelsOf(format);
    const usize texels = out.texels.Size() / Channels;
    const usize texelBytes = TextureSourceTexelBytes(format);
    // 8비트 색 표(디코딩 방식마다 256개 — 같은 값이면 같은 float).
    float table[256];
    if (source == SourceClass::Unorm8) {
        for (uint32 value = 0; value < 256; ++value) {
            table[value] = decode == Decode::Srgb
                               ? deterministic::SrgbByteToLinear(static_cast<uint8>(value))
                               : DecodeChannel(value, Unorm8Max, decode);
        }
    }

    for (usize index = 0; index < texels; ++index) {
        const uint8* texel = pixels.Data() + index * texelBytes;
        float color[Channels] = {0.0f, 0.0f, 0.0f, 1.0f};
        switch (source) {
        case SourceClass::Unorm8:
            for (uint32 channel = 0; channel < channels; ++channel) {
                const bool alpha = (channels == 2 && channel == 1) || channel == 3;
                color[alpha ? Alpha : channel] =
                    alpha ? static_cast<float>(static_cast<double>(texel[channel]) / Unorm8Max)
                          : table[texel[channel]];
            }

            break;
        case SourceClass::Unorm16:
            for (uint32 channel = 0; channel < channels; ++channel) {
                const bool alpha = (channels == 2 && channel == 1) || channel == 3;
                const uint32 value = LoadLe16(texel + channel * 2);
                color[alpha ? Alpha : channel] =
                    alpha ? static_cast<float>(static_cast<double>(value) / Unorm16Max)
                          : DecodeChannel(value, Unorm16Max, decode);
            }

            break;
        case SourceClass::Float: {
            const bool half =
                format == TextureSourceFormat::R16F || format == TextureSourceFormat::RGBA16F;
            for (uint32 channel = 0; channel < channels; ++channel) {
                color[channel] = half ? Float16::FromBits(LoadLe16(texel + channel * 2)).ToFloat()
                                      : platform::BitCast<float>(LoadLe32(texel + channel * 4));
            }

            break;
        }
        }

        // 회색(G8·GA8·G16·GA16)은 r = g = b다. R16F·R32F는 (v, 0, 0, 1)이다(UE CopyImage — 12.14절
        // A5j2b에서 고쳤다).
        if (channels <= 2 && source != SourceClass::Float) {
            color[Green] = color[Red];
            color[Blue] = color[Red];
        }

        float* target = out.texels.Data() + index * Channels;
        for (usize channel = 0; channel < Channels; ++channel) {
            target[channel] = color[channel];
        }
    }

    return Result<void>::Success();
}

Result<void> DecodeSource(const TextureBuildSettings& settings, Span<const uint8> pixels,
                          Level& out) noexcept {
    const bool volume = settings.dimension == TextureDimension::Volume;
    const Decode decode = DecodeFor(settings, ClassOf(settings.format));
    return DecodeFormat(settings.format, decode, settings.sizeX, settings.sizeY,
                        volume ? settings.numSlices : 1, volume ? 1 : settings.numSlices, pixels,
                        out);
}

// ---- 3. PowerOfTwoMode·재표본(12.5절 3번·크기 조정 필터) ----

uint32 RoundUpPowerOfTwo32(uint32 value) noexcept {
    uint32 power = 1;
    while (power < value) {
        power <<= 1;
    }

    return power;
}

// Mitchell–Netravali 커널이다(B·C — Catmull-Rom 0·½, Mitchell ⅓·⅓, 지지 반경 2).
struct CubicKernel {
    double b = 0.0;
    double c = 0.0;
};

constexpr CubicKernel CatmullRom{0.0, 0.5};
constexpr CubicKernel Mitchell{1.0 / 3.0, 1.0 / 3.0};
constexpr double CubicSupport = 2.0;

double CubicWeight(const CubicKernel& kernel, double x) noexcept {
    const double ax = x < 0.0 ? -x : x;
    const double ax2 = ax * ax;
    const double ax3 = ax2 * ax;
    const double b = kernel.b;
    const double c = kernel.c;
    if (ax < 1.0) {
        const double t3 = (12.0 - 9.0 * b - 6.0 * c) * ax3;
        const double t2 = (-18.0 + 12.0 * b + 6.0 * c) * ax2;
        const double t0 = 6.0 - 2.0 * b;
        return (t3 + t2 + t0) / 6.0;
    }

    if (ax < CubicSupport) {
        const double t3 = (-b - 6.0 * c) * ax3;
        const double t2 = (6.0 * b + 30.0 * c) * ax2;
        const double t1 = (-12.0 * b - 48.0 * c) * ax;
        const double t0 = 8.0 * b + 24.0 * c;
        return (t3 + t2 + t1 + t0) / 6.0;
    }

    return 0.0;
}

// 한 축 재표본 가중치 표다(대상 위치마다 시작 원본 위치와 정규화 가중치 — stride칸).
struct AxisFilter {
    explicit AxisFilter(Allocator& allocator) noexcept
        : starts(allocator), counts(allocator), weights(allocator) {
    }

    Array<int64> starts;
    Array<uint32> counts;
    Array<double> weights;
    usize stride = 0;
};

// AdaptiveSharp: 확대면 Catmull-Rom, 축소면 Mitchell, 같은 길이 축은 point 표본(복사 — UE ImageCore
// MapFilterToStb의 FilterIsNopWhenSameSize).
Result<void> MakeAxisFilter(uint32 source, uint32 target, AxisFilter& out) noexcept {
    if (source == target) {
        out.stride = 1;
        Result<void> step = out.starts.TryResize(target);
        if (step) {
            step = out.counts.TryResize(target);
        }

        if (step) {
            step = out.weights.TryResize(target);
        }

        if (!step) {
            return step;
        }

        for (uint32 i = 0; i < target; ++i) {
            out.starts[i] = i;
            out.counts[i] = 1;
            out.weights[i] = 1.0;
        }

        return Result<void>::Success();
    }

    const CubicKernel& kernel = source < target ? CatmullRom : Mitchell;
    const double ratio = static_cast<double>(source) / static_cast<double>(target);
    const double scale = target < source ? static_cast<double>(target) / source : 1.0;
    const double support = CubicSupport / scale;
    out.stride = static_cast<usize>(floor(2.0 * support)) + 2;
    Result<void> step = out.starts.TryResize(target);
    if (step) {
        step = out.counts.TryResize(target);
    }

    if (step) {
        step = out.weights.TryResize(usize{target} * out.stride);
    }

    if (!step) {
        return step;
    }

    for (uint32 i = 0; i < target; ++i) {
        const double scaled = (static_cast<double>(i) + 0.5) * ratio;
        const double center = scaled - 0.5;
        const int64 first = static_cast<int64>(ceil(center - support));
        const int64 last = static_cast<int64>(floor(center + support));
        double* weights = out.weights.Data() + usize{i} * out.stride;
        double total = 0.0;
        uint32 count = 0;
        for (int64 j = first; j <= last && count < out.stride; ++j) {
            const double distance = (static_cast<double>(j) - center) * scale;
            const double weight = CubicWeight(kernel, distance);
            weights[count++] = weight;
            total += weight;
        }

        for (uint32 tap = 0; tap < count; ++tap) {
            weights[tap] = total != 0.0 ? weights[tap] / total : 0.0;
        }

        out.starts[i] = first;
        out.counts[i] = count;
    }

    return Result<void>::Success();
}

int64 ClampIndex(int64 index, int64 n) noexcept {
    return index < 0 ? 0 : (index >= n ? n - 1 : index);
}

// level의 각 평면(layer·깊이 slice)을 (targetX, targetY)로 재표본한다(X 먼저, 그다음 Y, 가장자리
// clamp). 결과를 자르지 않는다(UE float 재표본 — 자르기는 출력 양자화 한 번뿐).
Result<void> Resample(Allocator& allocator, Level& level, uint32 targetX, uint32 targetY) noexcept {
    if (targetX == level.width && targetY == level.height) {
        return Result<void>::Success();
    }

    AxisFilter filterX(allocator);
    AxisFilter filterY(allocator);
    if (auto made = MakeAxisFilter(level.width, targetX, filterX); !made) {
        return made;
    }

    if (auto made = MakeAxisFilter(level.height, targetY, filterY); !made) {
        return made;
    }

    Level wide(allocator);
    if (auto made = wide.Allocate(targetX, level.height, level.depth, level.layers); !made) {
        return made;
    }

    for (uint32 layer = 0; layer < level.layers; ++layer) {
        for (uint32 z = 0; z < level.depth; ++z) {
            for (uint32 y = 0; y < level.height; ++y) {
                for (uint32 x = 0; x < targetX; ++x) {
                    const double* weights = filterX.weights.Data() + usize{x} * filterX.stride;
                    double sum[Channels] = {};
                    for (uint32 tap = 0; tap < filterX.counts[x]; ++tap) {
                        const int64 index = ClampIndex(filterX.starts[x] + tap, level.width);
                        const float* texel = level.At(layer, z, y, static_cast<uint32>(index));
                        for (usize channel = 0; channel < Channels; ++channel) {
                            const double term = weights[tap] * static_cast<double>(texel[channel]);
                            sum[channel] += term;
                        }
                    }

                    float* target = wide.At(layer, z, y, x);
                    for (usize channel = 0; channel < Channels; ++channel) {
                        target[channel] = static_cast<float>(sum[channel]);
                    }
                }
            }
        }
    }

    // X 단계가 끝났으니 원본을 놓는다(작업 메모리가 상한의 두 배를 넘지 않게).
    level.texels = Array<float>(allocator);
    Level tall(allocator);
    if (auto made = tall.Allocate(targetX, targetY, level.depth, level.layers); !made) {
        return made;
    }

    for (uint32 layer = 0; layer < level.layers; ++layer) {
        for (uint32 z = 0; z < level.depth; ++z) {
            for (uint32 y = 0; y < targetY; ++y) {
                const double* weights = filterY.weights.Data() + usize{y} * filterY.stride;
                for (uint32 x = 0; x < targetX; ++x) {
                    double sum[Channels] = {};
                    for (uint32 tap = 0; tap < filterY.counts[y]; ++tap) {
                        const int64 index = ClampIndex(filterY.starts[y] + tap, level.height);
                        const float* texel = wide.At(layer, z, static_cast<uint32>(index), x);
                        for (usize channel = 0; channel < Channels; ++channel) {
                            const double term = weights[tap] * static_cast<double>(texel[channel]);
                            sum[channel] += term;
                        }
                    }

                    float* target = tall.At(layer, z, y, x);
                    for (usize channel = 0; channel < Channels; ++channel) {
                        target[channel] = static_cast<float>(sum[channel]);
                    }
                }
            }
        }
    }

    level.width = tall.width;
    level.height = tall.height;
    level.texels = rocks::Move(tall.texels);
    return Result<void>::Success();
}

// 각 평면을 (targetX, targetY)로, Volume이면 깊이를 targetZ로 늘린다(UE Pad — 테두리면 행의 마지막
// texel·위 행·앞 slice, 아니면 채움 색).
Result<void> Pad(Allocator& allocator, Level& level, uint32 targetX, uint32 targetY, uint32 targetZ,
                 bool borderColor, const float* fill) noexcept {
    Level padded(allocator);
    if (auto made = padded.Allocate(targetX, targetY, targetZ, level.layers); !made) {
        return made;
    }

    for (uint32 layer = 0; layer < level.layers; ++layer) {
        for (uint32 z = 0; z < targetZ; ++z) {
            for (uint32 y = 0; y < targetY; ++y) {
                for (uint32 x = 0; x < targetX; ++x) {
                    const bool inside = x < level.width && y < level.height && z < level.depth;
                    const float* source = fill;
                    if (inside || borderColor) {
                        const uint32 sx = x < level.width ? x : level.width - 1;
                        const uint32 sy = y < level.height ? y : level.height - 1;
                        const uint32 sz = z < level.depth ? z : level.depth - 1;
                        source = level.At(layer, sz, sy, sx);
                    }

                    float* target = padded.At(layer, z, y, x);
                    for (usize channel = 0; channel < Channels; ++channel) {
                        target[channel] = source[channel];
                    }
                }
            }
        }
    }

    level.width = targetX;
    level.height = targetY;
    level.depth = targetZ;
    level.texels = rocks::Move(padded.texels);
    return Result<void>::Success();
}

} // namespace build_resize

Result<void> DecodeTexels(TextureSourceFormat format, TexelGamma gamma, uint32 width, uint32 height,
                          uint32 depth, uint32 layers, Span<const uint8> pixels,
                          Level& out) noexcept {
    using namespace build_resize;
    const uint64 texelBytes = TextureSourceTexelBytes(format);
    const uint64 texels = uint64{width} * height * depth * layers;
    if (texelBytes == 0 || pixels.Size() != texels * texelBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 감마는 8비트 정수 원본에만 뜻이 있다(16비트·float는 선형 — DecodeFormat이 본다).
    const Decode decode = gamma == TexelGamma::Srgb    ? Decode::Srgb
                          : gamma == TexelGamma::Pow22 ? Decode::Gamma22
                                                       : Decode::Linear;
    return DecodeFormat(format, ClassOf(format) == SourceClass::Unorm8 ? decode : Decode::Linear,
                        width, height, depth, layers, pixels, out);
}

Result<void> ResampleLevel(Allocator& allocator, Level& level, uint32 targetX,
                           uint32 targetY) noexcept {
    using namespace build_resize;
    return Resample(allocator, level, targetX, targetY);
}

} // namespace rocks::texture_build_detail
