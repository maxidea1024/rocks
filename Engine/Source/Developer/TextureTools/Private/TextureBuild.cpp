#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Memory.h>
#include "TextureBuildInternal.h"
#include "TextureImportInternal.h"

#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureBuild.h>
#include <TextureTools/TextureFormatSelect.h>
#include <TextureTools/TextureImport.h>

#include <math.h>

// 텍스처 빌드 구현이다(Docs/Design/AssetSystem 12.13절 — 규칙 12.5절·12.2절 1–5번, 출력 형식은
// 12.17.5절, UE 5.8 TextureCompressorModule.cpp와 같은 순서). 작업 픽셀은 float32 RGBA다. mip
// 평균·테두리는 UE와 같은 float 식, 재표본은 double 누적 후 float 저장이다. 곱과 합은 따로 써서
// FMA로 합쳐지지 않게 한다. 초월 함수는 DeterministicMath만 쓴다.
namespace rocks {

using graphics::PixelFormat;
namespace {

using texture_build_detail::Alpha;
using texture_build_detail::Blue;
using texture_build_detail::Channels;
using texture_build_detail::Green;
using texture_build_detail::Level;
using texture_build_detail::Red;
// 최대 크기 상한이다(12.5절 4번 — UE GetMaximumDimensionOfNonVT).
constexpr uint32 MaxDimension2D = 16384;
// 16비트 정수·16F 상한이다.
constexpr double Unorm8Max = 255.0;
constexpr double Unorm16Max = 65535.0;
constexpr float Half16Max = 65504.0f;
// 숨김 CompressionSettings다(UE TC_LQ·TC_EncodedReflectionCapture — 12.4.1절).
constexpr uint8 HiddenCompressionLq = 13;
constexpr uint8 HiddenCompressionReflection = 14;

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

Result<void> Fail(TextureBuildResult& out, AssetDiagnosticCode code, ErrorCode error,
                  uint64 detail) noexcept {
    out.failure.code = code;
    out.failure.error = error;
    out.failure.detail = detail;
    return Result<void>::Failure(error);
}

Result<void> Warn(TextureBuildResult& out, const TextureBuildWarning& added) noexcept {
    for (const TextureBuildWarning& warning : out.warnings) {
        if (warning.code == added.code) {
            return Result<void>::Success();
        }
    }

    if (auto pushed = out.warnings.TryPushBack(added); !pushed) {
        return Fail(out, AssetDiagnosticCode::None, pushed.Error(), 0);
    }

    return Result<void>::Success();
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

// ---- 5. mip 생성(12.5절 mip 규칙 — UE GenerateMip2x2·GenerateMipUnfiltered·GenerateMipBorder)
// ----

uint32 Half(uint32 n) noexcept {
    return n / 2 > 0 ? n / 2 : 1;
}

// 2×2 상자 하나다(UE ((A + B) + C) + D) × 0.25 — 원본 밖은 clamp, 길이 1인 축에서만 생긴다).
void Box2x2(const Level& source, uint32 layer, uint32 z, uint32 x, uint32 y, float* out) noexcept {
    const uint32 x0 = 2 * x < source.width ? 2 * x : source.width - 1;
    const uint32 x1 = 2 * x + 1 < source.width ? 2 * x + 1 : source.width - 1;
    const uint32 y0 = 2 * y < source.height ? 2 * y : source.height - 1;
    const uint32 y1 = 2 * y + 1 < source.height ? 2 * y + 1 : source.height - 1;
    const float* a = source.At(layer, z, y0, x0);
    const float* b = source.At(layer, z, y0, x1);
    const float* c = source.At(layer, z, y1, x0);
    const float* d = source.At(layer, z, y1, x1);
    for (usize channel = 0; channel < Channels; ++channel) {
        const float ab = a[channel] + b[channel];
        const float abc = ab + c[channel];
        const float sum = abc + d[channel];
        out[channel] = sum * 0.25f;
    }
}

// source의 다음 mip을 out에 만든다(Volume이면 slice 2z·2z + 1의 2D 결과를 (a + b) × 0.5로 평균).
Result<void> NextMip(const Level& source, bool volume, bool unfiltered, Level& out) noexcept {
    const uint32 width = Half(source.width);
    const uint32 height = Half(source.height);
    const uint32 depth = volume ? Half(source.depth) : source.depth;
    if (auto made = out.Allocate(width, height, depth, source.layers); !made) {
        return made;
    }

    for (uint32 layer = 0; layer < source.layers; ++layer) {
        for (uint32 z = 0; z < depth; ++z) {
            const uint32 z0 = volume ? (2 * z < source.depth ? 2 * z : source.depth - 1) : z;
            const bool second = volume && !unfiltered && source.depth > 1;
            const uint32 z1 = 2 * z + 1 < source.depth ? 2 * z + 1 : source.depth - 1;
            for (uint32 y = 0; y < height; ++y) {
                for (uint32 x = 0; x < width; ++x) {
                    float* target = out.At(layer, z, y, x);
                    if (unfiltered) {
                        const uint32 sx = 2 * x < source.width ? 2 * x : source.width - 1;
                        const uint32 sy = 2 * y < source.height ? 2 * y : source.height - 1;
                        const float* texel = source.At(layer, z0, sy, sx);
                        for (usize channel = 0; channel < Channels; ++channel) {
                            target[channel] = texel[channel];
                        }

                        continue;
                    }

                    Box2x2(source, layer, z0, x, y, target);
                    if (second) {
                        float other[Channels];
                        Box2x2(source, layer, z1, x, y, other);
                        for (usize channel = 0; channel < Channels; ++channel) {
                            const float sum = target[channel] + other[channel];
                            target[channel] = sum * 0.5f;
                        }
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

// 테두리 texel을 직전 mip 2×2 발자국 안의 원본 테두리 texel 평균으로 다시 쓴다(UE GenerateMipBorder
// — float 합 / 개수, 원본 밖은 clamp, Volume은 slice 2z).
void RedrawBorder(const Level& source, bool volume, Level& mip) noexcept {
    for (uint32 layer = 0; layer < mip.layers; ++layer) {
        for (uint32 z = 0; z < mip.depth; ++z) {
            const uint32 sz = volume ? (2 * z < source.depth ? 2 * z : source.depth - 1) : z;
            for (uint32 y = 0; y < mip.height; ++y) {
                for (uint32 x = 0; x < mip.width; ++x) {
                    const bool border =
                        x == 0 || y == 0 || x + 1 == mip.width || y + 1 == mip.height;
                    if (!border) {
                        continue;
                    }

                    float sum[Channels] = {};
                    float weight = 0.0f;
                    for (uint32 ky = 0; ky < 2; ++ky) {
                        for (uint32 kx = 0; kx < 2; ++kx) {
                            const uint32 sx = 2 * x + kx;
                            const uint32 sy = 2 * y + ky;
                            if (sx != 0 && sx + 1 != source.width && sy != 0 &&
                                sy + 1 != source.height) {
                                continue;
                            }

                            const uint32 cx = sx < source.width ? sx : source.width - 1;
                            const uint32 cy = sy < source.height ? sy : source.height - 1;
                            const float* texel = source.At(layer, sz, cy, cx);
                            for (usize channel = 0; channel < Channels; ++channel) {
                                sum[channel] += texel[channel];
                            }

                            weight += 1.0f;
                        }
                    }

                    float* target = mip.At(layer, z, y, x);
                    for (usize channel = 0; channel < Channels; ++channel) {
                        target[channel] = sum[channel] / weight;
                    }
                }
            }
        }
    }
}

// 법선 재정규화다(UE NormalizeMip — float v × 2 − 1을 double로, 제곱합 1은 그대로, 1e−8 미만은
// (0, 0, 1), 그 밖 1 / sqrt(제곱합)을 곱해 float(v × 0.5 + 0.5)로, 알파는 그대로).
constexpr double NormalSmallNumber = 1.0e-8;

void NormalizeNormals(Level& level) noexcept {
    const usize texels = level.texels.Size() / Channels;
    for (usize index = 0; index < texels; ++index) {
        float* texel = level.texels.Data() + index * Channels;
        double v[3];
        for (usize channel = 0; channel < 3; ++channel) {
            const float scaled = texel[channel] * 2.0f;
            v[channel] = static_cast<double>(scaled - 1.0f);
        }

        const double xx = v[0] * v[0];
        const double yy = v[1] * v[1];
        const double zz = v[2] * v[2];
        const double square = xx + yy + zz;
        if (square == 1.0) {
            // 그대로(UE GetSafeNormal).
        } else if (square < NormalSmallNumber) {
            v[0] = 0.0;
            v[1] = 0.0;
            v[2] = 1.0;
        } else {
            const double scale = 1.0 / sqrt(square);
            for (double& component : v) {
                component *= scale;
            }
        }

        for (usize channel = 0; channel < 3; ++channel) {
            const double halved = v[channel] * 0.5;
            texel[channel] = static_cast<float>(halved + 0.5);
        }
    }
}

// ---- 6. 출력(12.2절 3·5번) ----

// UE FColor::QuantizeUNormFloatTo8·16이다((uint)(0.5f + Clamp01NansTo0(v) × 최대) — float 식).
float Clamp01NansTo0(float value) noexcept {
    return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
}

uint8 QuantizeUnorm8(float value) noexcept {
    const float scaled = Clamp01NansTo0(value) * 255.0f;
    return static_cast<uint8>(0.5f + scaled);
}

uint16 QuantizeUnorm16(float value) noexcept {
    const float scaled = Clamp01NansTo0(value) * 65535.0f;
    return static_cast<uint16>(0.5f + scaled);
}

uint16 QuantizeHalf(float value) noexcept {
    if (value != value) {
        return 0;
    }

    const float clamped = value > Half16Max ? Half16Max : (value < -Half16Max ? -Half16Max : value);
    return Float16::FromFloat(clamped).bits;
}

float QuantizeSingle(float value) noexcept {
    if (value != value) {
        return 0.0f;
    }

    const float maximum = NumericLimits<float>::Max();
    return value > maximum ? maximum : (value < -maximum ? -maximum : value);
}

void StoreLe16(uint8* data, uint16 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
}

void StoreLe32(uint8* data, uint32 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
    data[2] = static_cast<uint8>(value >> 16);
    data[3] = static_cast<uint8>(value >> 24);
}

// 8비트 색 채널 하나다(sRGB 형식이면 sRGB 부호화 — 알파는 늘 선형).
uint8 EncodeUnorm8(float value, bool srgb) noexcept {
    return srgb ? texture_import::LinearToSrgb8(value) : QuantizeUnorm8(value);
}

TextureBlockFormat BlockFormatOf(TextureFormatEncoding encoding) noexcept {
    switch (encoding) {
    case TextureFormatEncoding::Bc3:
        return TextureBlockFormat::Bc3;
    case TextureFormatEncoding::Bc4:
        return TextureBlockFormat::Bc4;
    case TextureFormatEncoding::Bc5:
        return TextureBlockFormat::Bc5;
    default:
        return TextureBlockFormat::Bc1;
    }
}

// 출력 부호화 선택이다(12.17.5 3).
struct EncodeOptions {
    // HDR_Compressed의 RGBA16F 출력은 알파 1이다(UE BC6H는 알파가 없다).
    bool opaqueAlpha = false;
    // norm16 대체(G16 → R16F)는 Unorm 규칙대로 [0, 1]로 자르고 NaN → 0이다(12.14절).
    bool clampUnit = false;
};

// level을 choice 형식으로 부호화한다(slice·깊이 평면을 차례로, 행 사이 패딩 없음). BC는 RGBA8(같은
// sRGB 부호화)을 만들어 EncodeTextureBlocks로 압축한다.
Result<void> Encode(Allocator& allocator, const Level& level, const TextureFormatChoice& choice,
                    const EncodeOptions& options, Array<uint8>& out) noexcept {
    const usize texels = level.texels.Size() / Channels;
    const bool srgb = choice.srgb;
    if (choice.encoding != TextureFormatEncoding::Uncompressed) {
        Array<uint8> rgba(allocator);
        if (auto sized = rgba.TryResize(texels * TextureBlockTexelBytes); !sized) {
            return sized;
        }

        for (usize index = 0; index < texels; ++index) {
            const float* texel = level.texels.Data() + index * Channels;
            uint8* target = rgba.Data() + index * TextureBlockTexelBytes;
            target[0] = EncodeUnorm8(texel[Red], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Blue], srgb);
            target[3] = QuantizeUnorm8(texel[Alpha]);
        }

        const TextureBlockFormat block = BlockFormatOf(choice.encoding);
        const uint32 planes = level.layers * level.depth;
        auto bytes = TextureBlockOutputBytes(block, level.width, level.height, planes);
        if (!bytes) {
            return Result<void>::Failure(bytes.Error());
        }

        if (auto sized = out.TryResize(static_cast<usize>(bytes.Value())); !sized) {
            return sized;
        }

        TextureBlockImage image;
        image.rgba = rgba.View();
        image.sizeX = level.width;
        image.sizeY = level.height;
        image.numSlices = planes;
        return EncodeTextureBlocks(block, image, out.View());
    }

    const graphics::PixelFormat format = choice.pixelFormat;
    const usize texelBytes = graphics::GetPixelFormatInfo(format).blockBytes;
    if (texelBytes == 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (auto sized = out.TryResize(texels * texelBytes); !sized) {
        return sized;
    }

    for (usize index = 0; index < texels; ++index) {
        const float* texel = level.texels.Data() + index * Channels;
        uint8* target = out.Data() + index * texelBytes;
        const float alpha = options.opaqueAlpha ? 1.0f : texel[Alpha];
        switch (format) {
        case graphics::PixelFormat::R8Unorm:
            target[0] = QuantizeUnorm8(texel[Red]);
            break;
        case graphics::PixelFormat::R16Unorm:
            StoreLe16(target, QuantizeUnorm16(texel[Red]));
            break;
        case graphics::PixelFormat::R16Float:
            StoreLe16(target,
                      QuantizeHalf(options.clampUnit ? Clamp01NansTo0(texel[Red]) : texel[Red]));
            break;
        case graphics::PixelFormat::R32Float:
            StoreLe32(target, platform::BitCast<uint32>(QuantizeSingle(texel[Red])));
            break;
        case graphics::PixelFormat::BGRA8Unorm:
        case graphics::PixelFormat::BGRA8UnormSrgb:
            target[0] = EncodeUnorm8(texel[Blue], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Red], srgb);
            target[3] = QuantizeUnorm8(alpha);
            break;
        case graphics::PixelFormat::RGBA8Unorm:
        case graphics::PixelFormat::RGBA8UnormSrgb:
            target[0] = EncodeUnorm8(texel[Red], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Blue], srgb);
            target[3] = QuantizeUnorm8(alpha);
            break;
        case graphics::PixelFormat::RGBA16Float:
            for (uint32 c = 0; c < Channels; ++c) {
                StoreLe16(target + c * 2, QuantizeHalf(c == Alpha ? alpha : texel[c]));
            }

            break;
        case graphics::PixelFormat::RGBA32Float:
            for (uint32 c = 0; c < Channels; ++c) {
                StoreLe32(target + c * 4,
                          platform::BitCast<uint32>(QuantizeSingle(c == Alpha ? alpha : texel[c])));
            }

            break;
        default:
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    return Result<void>::Success();
}

// 채널 복제(12.17절 TD6 — UE ReplicateRedChannel·ReplicateAlphaChannel, 선형 float).
void Replicate(Level& level, TextureChannelReplicate replicate) noexcept {
    if (replicate == TextureChannelReplicate::None) {
        return;
    }

    const usize source = replicate == TextureChannelReplicate::Red ? Red : Alpha;
    for (usize index = 0; index < level.texels.Size(); index += Channels) {
        const float value = level.texels[index + source];
        for (usize c = 0; c < Channels; ++c) {
            level.texels[index + c] = value;
        }
    }
}

// 알파 판정(UE DetectAlphaChannel의 8비트 문턱 — 최소 알파 ≤ 254.5 / 255면 투명)이다.
bool HasTransparentAlpha(const Level& level) noexcept {
    constexpr float Threshold = 254.5f / 255.0f;
    for (usize index = Alpha; index < level.texels.Size(); index += Channels) {
        if (level.texels[index] <= Threshold) {
            return true;
        }
    }

    return false;
}

// ---- 7. 쿠킹 편향(12.5절 7번 — UE CalculateLODBias) ----

uint32 FloorLog2(uint32 value) noexcept {
    uint32 log = 0;
    while (value > 1) {
        value >>= 1;
        ++log;
    }

    return log;
}

// UE FMath::CeilLogTwo다(0과 1은 0).
int32 CeilLog2(int32 value) noexcept {
    if (value <= 1) {
        return 0;
    }

    int32 log = 0;
    int64 power = 1;
    while (power < value) {
        power <<= 1;
        ++log;
    }

    return log;
}

// UE FMath::Clamp(x < 최소면 최소, 아니면 min(x, 최대))다.
int32 UeClamp(int32 value, int32 minimum, int32 maximum) noexcept {
    return value < minimum ? minimum : (value < maximum ? value : maximum);
}

// 버릴 상위 mip 수다(W·H = 사슬 mip 0의 X·Y, chain = 사슬 길이).
uint32 CookDroppedMips(const TextureBuildSettings& settings, uint32 width, uint32 height,
                       uint32 chain) noexcept {
    if (settings.mipGenSettings == TextureMipGenSettings::NoMipmaps) {
        return 0;
    }

    const int32 textureMaxLod = static_cast<int32>(FloorLog2(width > height ? width : height));
    // textureMaxLod − bias를 int64로 구해 int32 범위로 자른다(넘침 없이).
    const int64 lowered = int64{textureMaxLod} - settings.lodBias;
    const int32 target = lowered > NumericLimits<int32>::Max()   ? NumericLimits<int32>::Max()
                         : lowered < NumericLimits<int32>::Min() ? NumericLimits<int32>::Min()
                                                                 : static_cast<int32>(lowered);
    int32 wanted = UeClamp(target, CeilLog2(settings.minLodSize), CeilLog2(settings.maxLodSize));
    wanted = UeClamp(wanted, 0, textureMaxLod);
    const uint32 dropped = static_cast<uint32>(textureMaxLod - wanted);
    return dropped < chain - 1 ? dropped : chain - 1;
}

bool IsHidden(TextureCompressionSettings compression) noexcept {
    const uint8 value = static_cast<uint8>(compression);
    return value == HiddenCompressionLq || value == HiddenCompressionReflection;
}

// 크기 계획이다(BuildTexture와 TextureBuildTopMipSize가 같은 식을 쓴다): PowerOfTwoMode 대상, 최대
// 크기를 맞춘 mip 0, 사슬 길이, 버릴 상위 mip, 쿠킹 상위 mip.
struct SizePlan {
    uint32 targetX = 0;
    uint32 targetY = 0;
    uint32 targetZ = 1;
    bool pad = false;
    uint32 fitX = 0;
    uint32 fitY = 0;
    uint32 fitZ = 1;
    uint32 chain = 1;
    uint32 dropped = 0;
    uint32 topX = 0;
    uint32 topY = 0;
};

SizePlan PlanSizes(const TextureBuildSettings& settings) noexcept {
    SizePlan plan;
    const bool volume = settings.dimension == TextureDimension::Volume;
    const TexturePowerOfTwoSetting mode = settings.powerOfTwoMode;
    const uint32 depth = volume ? settings.numSlices : 1;
    plan.targetX = settings.sizeX;
    plan.targetY = settings.sizeY;
    plan.targetZ = depth;
    plan.pad = mode == TexturePowerOfTwoSetting::PadToPowerOfTwo ||
               mode == TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
    const bool square = mode == TexturePowerOfTwoSetting::PadToSquarePowerOfTwo ||
                        mode == TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo;
    if (plan.pad || mode == TexturePowerOfTwoSetting::StretchToPowerOfTwo ||
        mode == TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo) {
        plan.targetX = RoundUpPowerOfTwo32(settings.sizeX);
        plan.targetY = RoundUpPowerOfTwo32(settings.sizeY);
        plan.targetZ = volume ? RoundUpPowerOfTwo32(depth) : depth;
        if (square) {
            uint32 larger = plan.targetX > plan.targetY ? plan.targetX : plan.targetY;
            larger = volume && plan.targetZ > larger ? plan.targetZ : larger;
            plan.targetX = larger;
            plan.targetY = larger;
            plan.targetZ = volume ? larger : depth;
        }

        // Stretch·Resize는 Volume Z를 바꾸지 않는다(12.5절 3번 — UE와 다름).
        if (!plan.pad) {
            plan.targetZ = depth;
        }
    } else if (mode == TexturePowerOfTwoSetting::ResizeToSpecificResolution) {
        plan.targetX = settings.resizeX != 0 ? settings.resizeX : settings.sizeX;
        plan.targetY = settings.resizeY != 0 ? settings.resizeY : settings.sizeY;
    }

    // 4. 최대 크기(UE MaxTextureResolution — X·Y만 본다, 2×2 단계 경로는 Volume Z도 반으로).
    uint64 maximum = MaxDimension2D;
    if (settings.maxTextureSize > 0 && static_cast<uint64>(settings.maxTextureSize) < maximum) {
        maximum = static_cast<uint64>(settings.maxTextureSize);
    }

    plan.fitX = plan.targetX;
    plan.fitY = plan.targetY;
    plan.fitZ = plan.targetZ;
    const bool stepPath = volume || settings.preserveBorder;
    while (plan.fitX > maximum || plan.fitY > maximum) {
        plan.fitX = Half(plan.fitX);
        plan.fitY = Half(plan.fitY);
        plan.fitZ = stepPath && volume ? Half(plan.fitZ) : plan.fitZ;
    }

    uint32 longest = plan.fitX > plan.fitY ? plan.fitX : plan.fitY;
    longest = volume && plan.fitZ > longest ? plan.fitZ : longest;
    plan.chain =
        settings.mipGenSettings == TextureMipGenSettings::NoMipmaps ? 1 : FloorLog2(longest) + 1;
    plan.dropped = CookDroppedMips(settings, plan.fitX, plan.fitY, plan.chain);
    plan.topX = plan.fitX;
    plan.topY = plan.fitY;
    for (uint32 mip = 0; mip < plan.dropped; ++mip) {
        plan.topX = Half(plan.topX);
        plan.topY = Half(plan.topY);
    }

    return plan;
}

// settings에서 형식 선택 입력을 만든다(12.17.5 3).
TextureFormatRequest FormatRequest(const TextureBuildSettings& settings,
                                   const SizePlan& plan) noexcept {
    TextureFormatRequest request;
    request.compression = settings.compression;
    request.srgb = settings.srgb;
    request.sourceFormat = settings.format;
    request.dimension = settings.dimension;
    request.lodGroup = settings.lodGroup;
    request.compressionNone = settings.compressionNone;
    request.compressionNoAlpha = settings.compressionNoAlpha;
    request.compressionForceAlpha = settings.compressionForceAlpha;
    request.pointFilter = settings.samplerPointFilter;
    request.topSizeX = plan.topX;
    request.topSizeY = plan.topY;
    return request;
}

TextureFormatTarget FormatTarget(const TextureBuildSettings& settings) noexcept {
    TextureFormatTarget target;
    target.platform = static_cast<graphics::GraphicsPlatform>(settings.cookPlatform);
    target.backends = settings.cookBackends;
    target.norm16Supported = settings.norm16Supported;
    return target;
}

bool ValidSize(const TextureBuildSettings& settings) noexcept {
    return settings.format != TextureSourceFormat::Invalid && settings.sizeX != 0 &&
           settings.sizeY != 0 && settings.numSlices != 0;
}

Result<void> ChooseFormat(const TextureBuildSettings& settings, bool alphaTransparent,
                          TextureFormatChoice& choice) noexcept {
    if (!ValidSize(settings) || IsHidden(settings.compression)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SizePlan plan = PlanSizes(settings);
    return SelectTextureFormat(FormatRequest(settings, plan), FormatTarget(settings),
                               alphaTransparent, choice);
}

EncodeOptions EncodeOptionsFor(const TextureBuildSettings& settings,
                               const TextureFormatChoice& choice) noexcept {
    EncodeOptions options;
    options.opaqueAlpha = settings.compression == TextureCompressionSettings::HDR_Compressed;
    options.clampUnit = choice.pixelFormat == graphics::PixelFormat::R16Float &&
                        settings.format == TextureSourceFormat::G16 && !settings.norm16Supported;
    return options;
}

// 작업 픽셀 byte 수가 상한 안인지다(넘침 검사 포함).
bool WithinWorkingBytes(uint64 x, uint64 y, uint64 z, uint64 layers) noexcept {
    const uint64 limit = TextureBuildMaxWorkingBytes / (Channels * sizeof(float));
    if (x == 0 || y == 0 || z == 0 || layers == 0) {
        return true;
    }

    if (x > limit || y > limit / x) {
        return false;
    }

    const uint64 plane = x * y;
    if (z > limit / plane) {
        return false;
    }

    const uint64 volume = plane * z;
    return layers <= limit / volume;
}

} // namespace

bool TextureCompressionIgnoresSrgb(TextureCompressionSettings compression) noexcept {
    // UE bPreventSRGB(Texture.cpp 772–777): Alpha·Normalmap·Masks와 HDR류(UE IsHDR)다.
    switch (compression) {
    case TextureCompressionSettings::Normalmap:
    case TextureCompressionSettings::Masks:
    case TextureCompressionSettings::Alpha:
    case TextureCompressionSettings::HDR:
    case TextureCompressionSettings::HDR_Compressed:
    case TextureCompressionSettings::HalfFloat:
    case TextureCompressionSettings::SingleFloat:
    case TextureCompressionSettings::HDR_F32:
        return true;
    default:
        return false;
    }
}

usize TextureBuildWarnings(const TextureBuildSettings& settings, bool alphaTransparent,
                           TextureBuildWarning (&out)[TextureBuildMaxWarnings]) noexcept {
    usize count = 0;
    TextureFormatChoice choice;
    const bool chosen = static_cast<bool>(ChooseFormat(settings, alphaTransparent, choice));
    if (chosen && choice.srgbIgnored) {
        out[count++] = TextureBuildWarning{AssetDiagnosticCode::SrgbIgnored, 0};
    }

    // PreserveBorder Wrap(쓰는 축 — Volume은 Z도, mip이 없으면 테두리를 다시 쓰지 않는다).
    const bool volume = settings.dimension == TextureDimension::Volume;
    if (settings.preserveBorder && settings.mipGenSettings != TextureMipGenSettings::NoMipmaps &&
        (settings.addressX == TextureAddress::Wrap || settings.addressY == TextureAddress::Wrap ||
         (volume && settings.addressZ == TextureAddress::Wrap))) {
        out[count++] = TextureBuildWarning{AssetDiagnosticCode::PreserveBorderWithWrap, 0};
    }

    for (usize index = 0; chosen && index < choice.warningCount; ++index) {
        out[count++] =
            TextureBuildWarning{choice.warnings[index].code, choice.warnings[index].detail};
    }

    return count;
}

TextureBuildOutputInfo TextureBuildOutput(const TextureBuildSettings& settings,
                                          bool alphaTransparent) noexcept {
    TextureBuildOutputInfo info;
    TextureFormatChoice choice;
    if (!ChooseFormat(settings, alphaTransparent, choice)) {
        return info;
    }

    info.pixelFormat = choice.pixelFormat;
    info.srgb = choice.srgb;
    for (usize index = 0; index < 4; ++index) {
        info.swizzle[index] = choice.swizzle[index];
    }

    return info;
}

Result<void> TextureBuildTopMipSize(const TextureBuildSettings& settings, uint32& sizeX,
                                    uint32& sizeY) noexcept {
    if (!ValidSize(settings)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SizePlan plan = PlanSizes(settings);
    sizeX = plan.topX;
    sizeY = plan.topY;
    return Result<void>::Success();
}

Result<void> ResolveTextureBuildSettings(const Texture& texture, TextureDimension dimension,
                                         const TextureLodGroupValues& group,
                                         TextureBuildSettings& out,
                                         TextureBuildResult& result) noexcept {
    result.failure = TextureBuildFailure{};
    const TextureSource& source = texture.source;
    if (source.format == TextureSourceFormat::Invalid || source.sizeX == 0 || source.sizeY == 0) {
        return Fail(result, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, 0);
    }

    const uint32 slices = source.numSlices;
    bool slicesOk = slices >= 1;
    switch (dimension) {
    case TextureDimension::Texture2D:
        slicesOk = slices == 1;
        break;
    case TextureDimension::TextureCube:
        slicesOk = slices == TextureCubeFaces;
        break;
    case TextureDimension::TextureCubeArray:
        slicesOk = slices >= TextureCubeFaces && slices % TextureCubeFaces == 0;
        break;
    default:
        break;
    }

    if (!slicesOk) {
        return Fail(result, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, slices);
    }

    if (IsHidden(texture.compressionSettings)) {
        return Fail(result, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(texture.compressionSettings));
    }

    if (texture.powerOfTwoMode == TexturePowerOfTwoSetting::ResizeToSpecificResolution &&
        dimension != TextureDimension::Texture2D) {
        return Fail(result, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(texture.powerOfTwoMode));
    }

    TextureMipGenSettings mipGen = texture.mipGenSettings;
    if (mipGen == TextureMipGenSettings::FromTextureGroup) {
        mipGen = group.mipGenSettings;
    }

    if (mipGen != TextureMipGenSettings::SimpleAverage &&
        mipGen != TextureMipGenSettings::NoMipmaps && mipGen != TextureMipGenSettings::Unfiltered) {
        return Fail(result, AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported,
                    static_cast<uint64>(mipGen));
    }

    // CPU에서 읽는 Texture2D는 mip이 없다(UE GetTextureBuildSettings).
    if (dimension == TextureDimension::Texture2D &&
        texture.availability == TextureAvailability::CPU) {
        mipGen = TextureMipGenSettings::NoMipmaps;
    }

    out = TextureBuildSettings{};
    out.dimension = dimension;
    out.format = source.format;
    out.sizeX = source.sizeX;
    out.sizeY = source.sizeY;
    out.numSlices = slices;
    out.compression = texture.compressionSettings;
    out.srgb = texture.srgb != 0;
    out.useLegacyGamma = texture.useLegacyGamma != 0;
    out.encodingOverride = texture.sourceColorSettings.encodingOverride;
    out.flipGreenChannel = texture.flipGreenChannel != 0;
    out.powerOfTwoMode = texture.powerOfTwoMode;
    out.paddingColor = texture.paddingColor;
    out.padWithBorderColor = texture.padWithBorderColor;
    out.resizeX =
        texture.resizeDuringBuildX > 0 ? static_cast<uint32>(texture.resizeDuringBuildX) : 0;
    out.resizeY =
        texture.resizeDuringBuildY > 0 ? static_cast<uint32>(texture.resizeDuringBuildY) : 0;
    out.mipGenSettings = mipGen;
    out.preserveBorder = texture.preserveBorder != 0;
    out.normalizeNormals = texture.normalizeNormals != 0 &&
                           texture.compressionSettings == TextureCompressionSettings::Normalmap;
    // 합은 int32 범위로 자른다(넘침 없이 — 편향은 사슬 길이로 다시 잘린다).
    const int64 bias = int64{texture.lodBias} + group.lodBias;
    out.lodBias = bias > NumericLimits<int32>::Max()   ? NumericLimits<int32>::Max()
                  : bias < NumericLimits<int32>::Min() ? NumericLimits<int32>::Min()
                                                       : static_cast<int32>(bias);
    out.maxTextureSize = texture.maxTextureSize;
    out.minLodSize = group.minLodSize;
    out.maxLodSize = group.maxLodSize;
    out.compressionNone = texture.compressionNone != 0;
    out.compressionNoAlpha = texture.compressionNoAlpha != 0;
    out.compressionForceAlpha = texture.compressionForceAlpha != 0;
    out.lodGroup = texture.lodGroup;
    // 주소(12.4.2절): texture는 dimension의 클래스다(호출자 계약 — 쿠킹이 타입으로 정한다).
    switch (dimension) {
    case TextureDimension::Texture2D: {
        const auto& plane = static_cast<const Texture2D&>(texture);
        out.addressX = plane.addressX;
        out.addressY = plane.addressY;
        out.addressZ = TextureAddress::Clamp;
        break;
    }
    case TextureDimension::Texture2DArray: {
        const auto& array = static_cast<const Texture2DArray&>(texture);
        out.addressX = array.addressX;
        out.addressY = array.addressY;
        out.addressZ = array.addressZ;
        break;
    }
    case TextureDimension::Volume: {
        const auto& volume = static_cast<const VolumeTexture&>(texture);
        out.addressX = volume.addressMode;
        out.addressY = volume.addressMode;
        out.addressZ = volume.addressMode;
        break;
    }
    default:
        out.addressX = TextureAddress::Clamp;
        out.addressY = TextureAddress::Clamp;
        out.addressZ = TextureAddress::Clamp;
        break;
    }

    return Result<void>::Success();
}

Result<void> BuildTexture(Allocator& allocator, const TextureBuildSettings& settings,
                          Span<const uint8> sourcePixels, TextureBuildResult& out) noexcept {
    out.mips.Clear();
    out.warnings.Clear();
    out.failure = TextureBuildFailure{};
    out.pixelFormat = graphics::PixelFormat::Unknown;
    out.srgb = false;
    const bool volume = settings.dimension == TextureDimension::Volume;
    const bool cube = settings.dimension == TextureDimension::TextureCube ||
                      settings.dimension == TextureDimension::TextureCubeArray;
    if (IsHidden(settings.compression)) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(settings.compression));
    }

    // 원본 검사(byte 수는 넘침 없이 — 각 인자가 32비트라 64비트 곱이 넘치지 않는다).
    const uint64 texelBytes = TextureSourceTexelBytes(settings.format);
    const uint64 plane = uint64{settings.sizeX} * settings.sizeY;
    const bool sized = settings.format != TextureSourceFormat::Invalid && settings.sizeX != 0 &&
                       settings.sizeY != 0 && settings.numSlices != 0 &&
                       plane <= NumericLimits<uint64>::Max() / settings.numSlices / texelBytes;
    if (!sized || plane * settings.numSlices * texelBytes != sourcePixels.Size()) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument,
                    sourcePixels.Size());
    }

    if (cube && settings.sizeX != settings.sizeY) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument,
                    settings.sizeX);
    }

    // 직접 채운 설정도 Resolve와 같은 규칙을 지킨다(slice 수·mipGen 셋).
    const uint32 slices = settings.numSlices;
    const bool slicesOk =
        settings.dimension == TextureDimension::Texture2D          ? slices == 1
        : settings.dimension == TextureDimension::TextureCube      ? slices == TextureCubeFaces
        : settings.dimension == TextureDimension::TextureCubeArray ? slices % TextureCubeFaces == 0
                                                                   : true;
    if (!slicesOk) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, slices);
    }

    if (settings.mipGenSettings != TextureMipGenSettings::SimpleAverage &&
        settings.mipGenSettings != TextureMipGenSettings::NoMipmaps &&
        settings.mipGenSettings != TextureMipGenSettings::Unfiltered) {
        return Fail(out, AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported,
                    static_cast<uint64>(settings.mipGenSettings));
    }

    // PowerOfTwoMode 대상 크기(UE GetPowerOfTwoTargetTextureSize — 작업 byte를 먼저 판정한다).
    const uint32 depth = volume ? settings.numSlices : 1;
    const uint32 layers = volume ? 1 : settings.numSlices;
    const SizePlan plan = PlanSizes(settings);
    const uint32 targetX = plan.targetX;
    const uint32 targetY = plan.targetY;
    const uint32 targetZ = plan.targetZ;
    const bool pad = plan.pad;
    if (cube && targetX != targetY) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(settings.powerOfTwoMode));
    }

    const uint32 widestX = targetX > settings.sizeX ? targetX : settings.sizeX;
    const uint32 widestY = targetY > settings.sizeY ? targetY : settings.sizeY;
    const uint32 deepest = targetZ > depth ? targetZ : depth;
    if (!WithinWorkingBytes(widestX, widestY, deepest, layers)) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded,
                    sourcePixels.Size());
    }

    // 형식 선택 입력이 맞는지 먼저 본다(대상 backend의 보장 대체가 없으면 InvalidSetting — TD12).
    const TextureFormatRequest request = FormatRequest(settings, plan);
    const TextureFormatTarget target = FormatTarget(settings);
    TextureFormatChoice choice;
    if (!SelectTextureFormat(request, target, false, choice)) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    settings.cookBackends);
    }

    // 1. 선형화.
    Level level(allocator);
    if (auto decoded = DecodeSource(settings, sourcePixels, level); !decoded) {
        return Fail(out, AssetDiagnosticCode::None, decoded.Error(), 0);
    }

    // 3. PowerOfTwoMode.
    if (targetX != level.width || targetY != level.height || targetZ != level.depth) {
        Result<void> changed = Result<void>::Success();
        if (pad) {
            // 채움 색은 8비트 sRGB로 디코딩한다(UE FLinearColor(FColor) — 원본 형식과 무관).
            const Color color = settings.paddingColor;
            const float fill[Channels] = {deterministic::SrgbByteToLinear(color.r),
                                          deterministic::SrgbByteToLinear(color.g),
                                          deterministic::SrgbByteToLinear(color.b),
                                          static_cast<float>(color.a) * (1.0f / 255.0f)};
            changed =
                Pad(allocator, level, targetX, targetY, targetZ, settings.padWithBorderColor, fill);
        } else {
            changed = Resample(allocator, level, targetX, targetY);
        }

        if (!changed) {
            return Fail(out, AssetDiagnosticCode::None, changed.Error(), 0);
        }
    }

    const bool unfiltered = settings.mipGenSettings == TextureMipGenSettings::Unfiltered;
    // 4. 최대 크기(UE MaxTextureResolution — X·Y만 본다).
    uint64 maximum = MaxDimension2D;
    if (settings.maxTextureSize > 0 && static_cast<uint64>(settings.maxTextureSize) < maximum) {
        maximum = static_cast<uint64>(settings.maxTextureSize);
    }

    if (level.width > maximum || level.height > maximum) {
        uint32 fitX = level.width;
        uint32 fitY = level.height;
        while (fitX > maximum || fitY > maximum) {
            fitX = Half(fitX);
            fitY = Half(fitY);
        }

        if (!volume && !settings.preserveBorder) {
            if (auto resized = Resample(allocator, level, fitX, fitY); !resized) {
                return Fail(out, AssetDiagnosticCode::None, resized.Error(), 0);
            }
        } else {
            while (level.width > maximum || level.height > maximum) {
                Level next(allocator);
                if (auto made = NextMip(level, volume, unfiltered, next); !made) {
                    return Fail(out, AssetDiagnosticCode::None, made.Error(), 0);
                }

                if (settings.preserveBorder) {
                    RedrawBorder(level, volume, next);
                }

                level = rocks::Move(next);
            }
        }
    }

    // flipGreen(UE — Pad·Stretch·최대 크기를 거친 mip 0에서 G = 1 − Clamp(G, 0, 1)).
    if (settings.flipGreenChannel) {
        for (usize index = 0; index < level.texels.Size(); index += Channels) {
            level.texels[index + Green] = 1.0f - Clamp01NansTo0(level.texels[index + Green]);
        }
    }

    const bool noMips = settings.mipGenSettings == TextureMipGenSettings::NoMipmaps;

    // 5. mip 사슬(Volume은 Z 포함 — NoMipmaps는 mip 0만).
    uint32 longest = level.width > level.height ? level.width : level.height;
    longest = volume && level.depth > longest ? level.depth : longest;
    const uint32 chain = noMips ? 1 : FloorLog2(longest) + 1;
    const uint32 dropped = CookDroppedMips(settings, level.width, level.height, chain);
    // 크기 계획과 실제 사슬이 같아야 한다(상위 mip 크기 규칙이 이 값으로 형식을 골랐다).
    if (chain != plan.chain || dropped != plan.dropped || level.width != plan.fitX ||
        level.height != plan.fitY) {
        return Fail(out, AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }
    if (auto reserved = out.mips.TryReserve(chain - dropped); !reserved) {
        return Fail(out, AssetDiagnosticCode::None, reserved.Error(), 0);
    }

    Array<Level> mips(allocator);
    if (auto reserved = mips.TryReserve(chain); !reserved) {
        return Fail(out, AssetDiagnosticCode::None, reserved.Error(), 0);
    }

    (void)mips.TryPushBack(rocks::Move(level));
    for (uint32 mip = 1; mip < chain; ++mip) {
        Level next(allocator);
        if (auto made = NextMip(mips[mip - 1], volume, unfiltered, next); !made) {
            return Fail(out, AssetDiagnosticCode::None, made.Error(), 0);
        }

        if (settings.preserveBorder) {
            RedrawBorder(mips[mip - 1], volume, next);
        }

        (void)mips.TryPushBack(rocks::Move(next));
    }

    // 6. NormalizeNormals(사슬을 다 만든 뒤 mip 0 포함 — UE NormalizeMip).
    if (settings.normalizeNormals) {
        for (Level& mip : mips) {
            NormalizeNormals(mip);
        }
    }

    // 6b. 채널 복제(UE — NormalizeNormals 뒤 모든 mip, 12.17절 TD6).
    for (Level& mip : mips) {
        Replicate(mip, choice.replicate);
    }

    // 6c. 알파 판정(AutoDXT일 때만 — 사슬의 mip 0, 쿠킹 편향으로 버리기 전, 12.17.5 3).
    bool alphaTransparent = false;
    if (TextureFormatDependsOnAlpha(request, target)) {
        alphaTransparent = HasTransparentAlpha(mips[0]);
        if (!SelectTextureFormat(request, target, alphaTransparent, choice)) {
            return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                        settings.cookBackends);
        }
    }

    TextureBuildWarning warnings[TextureBuildMaxWarnings];
    const usize warningCount = TextureBuildWarnings(settings, alphaTransparent, warnings);
    for (usize index = 0; index < warningCount; ++index) {
        if (auto warned = Warn(out, warnings[index]); !warned) {
            return warned;
        }
    }

    // 7·8. 쿠킹 편향과 출력.
    const EncodeOptions options = EncodeOptionsFor(settings, choice);
    for (uint32 mip = dropped; mip < chain; ++mip) {
        const Level& current = mips[mip];
        TextureBuiltMip built(allocator);
        built.sizeX = current.width;
        built.sizeY = current.height;
        built.sizeZ = volume ? current.depth : 1;
        if (auto encoded = Encode(allocator, current, choice, options, built.bytes); !encoded) {
            return Fail(out, AssetDiagnosticCode::None, encoded.Error(), 0);
        }

        (void)out.mips.TryPushBack(rocks::Move(built));
    }

    out.pixelFormat = choice.pixelFormat;
    out.srgb = choice.srgb;
    for (usize index = 0; index < 4; ++index) {
        out.swizzle[index] = choice.swizzle[index];
    }

    out.alphaTransparent = alphaTransparent;
    out.numSlices = volume ? 1 : settings.numSlices;
    return Result<void>::Success();
}

namespace texture_build_detail {

Result<void> DecodeTexels(TextureSourceFormat format, TexelGamma gamma, uint32 width, uint32 height,
                          uint32 depth, uint32 layers, Span<const uint8> pixels,
                          Level& out) noexcept {
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
    return Resample(allocator, level, targetX, targetY);
}

uint8 QuantizeUnorm8Value(float value) noexcept {
    return QuantizeUnorm8(value);
}

uint16 QuantizeUnorm16Value(float value) noexcept {
    return QuantizeUnorm16(value);
}

bool WithinTextureWorkingBytes(uint64 x, uint64 y, uint64 z, uint64 layers) noexcept {
    return WithinWorkingBytes(x, y, z, layers);
}

} // namespace texture_build_detail

} // namespace rocks
