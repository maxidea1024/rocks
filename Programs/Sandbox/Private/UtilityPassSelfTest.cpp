#include "UtilityPassSelfTest.h"
#include "EngineShaderComputeProbes.h"
#include "SandboxShaders.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <Core/Math/Float16.h>
#include <Graphics/ShaderBindings.h>
#include <Platform/Memory.h>
#include <RenderBase/ClearResources.h>
#include <RenderBase/DefaultTextureCache.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/GenerateMips.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/PipelineStatePresets.h>
#include <RenderBase/UtilityShaderTypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 엔진 셰이더 SL1 유틸리티 pass self-test다(Docs/Design/EngineShaders 14.6절 3항). 셰이더는
// Engine/Shaders/Private/Utility이고 도우미는
// RenderBase(GenerateMips·ClearResources·DefaultTextureCache)다.
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s utility pass: %s\n", backendName, what);
    }
}

void PrintDeviceError(Device& device) noexcept {
    printf("    device error: %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

double SrgbEncode(double x) noexcept {
    const double v = fmin(fmax(x, 0.0), 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

double SrgbDecode(double x) noexcept {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

// 집합 항목의 텍스처 slot이다(없으면 SetTexture가 무시하는 범위 밖 slot).
uint32 TextureSlotOf(render::ShaderSet& set, uint32 entry, StringView name) noexcept {
    const shader::ShaderBindingLayout* layout = set.BindingLayout(entry);
    auto slot = layout != nullptr ? graphics::FindTextureSlot(*layout, name)
                                  : Result<uint32>::Failure(ErrorCode::NotFound);
    return slot ? slot.Value() : MaxResourceSlots;
}

// --- 형식 부호화(시험용 — 읽은 byte의 값과 기대 값) ---

// 시험 형식 한 행이다(성분 수, texel byte, 종류).
enum class TexelKind : uint8 {
    Unorm8,
    Srgb8,
    Half,
    Float32,
    Uint32,
    Sint32,
    SmallFloat
};

struct TexelFormat {
    PixelFormat format;
    uint32 components;
    uint32 bytes;
    TexelKind kind;
};

constexpr TexelFormat TexelFormats[] = {{PixelFormat::RGBA8Unorm, 4, 4, TexelKind::Unorm8},
                                        {PixelFormat::RGBA8UnormSrgb, 4, 4, TexelKind::Srgb8},
                                        {PixelFormat::RGBA16Float, 4, 8, TexelKind::Half},
                                        {PixelFormat::RGBA32Float, 4, 16, TexelKind::Float32},
                                        {PixelFormat::R32Float, 1, 4, TexelKind::Float32},
                                        {PixelFormat::R32Uint, 1, 4, TexelKind::Uint32},
                                        {PixelFormat::R32Sint, 1, 4, TexelKind::Sint32},
                                        {PixelFormat::RGBA32Uint, 4, 16, TexelKind::Uint32},
                                        {PixelFormat::RG16Float, 2, 4, TexelKind::Half},
                                        {PixelFormat::R16Float, 1, 2, TexelKind::Half},
                                        {PixelFormat::RG32Float, 2, 8, TexelKind::Float32},
                                        {PixelFormat::RG11B10Ufloat, 3, 4, TexelKind::SmallFloat},
                                        {PixelFormat::RGBA8Snorm, 4, 4, TexelKind::Unorm8}};

const TexelFormat* FindTexelFormat(PixelFormat format) noexcept {
    for (const TexelFormat& texel : TexelFormats) {
        if (texel.format == format) {
            return &texel;
        }
    }

    return nullptr;
}

// 작은 float(부호 없음, 지수 5 bit, 가수 mantissaBits)의 값이다.
double DecodeSmallFloat(uint32 bits, uint32 mantissaBits) noexcept {
    const uint32 exponent = bits >> mantissaBits;
    const double mantissa =
        (bits & ((1u << mantissaBits) - 1u)) / static_cast<double>(1u << mantissaBits);
    return exponent == 0 ? ldexp(mantissa, -14)
                         : ldexp(1.0 + mantissa, static_cast<int>(exponent) - 15);
}

// texel byte → 성분 값(정수 형식은 정수 값, snorm은 [-1, 1])이다.
void DecodeTexel(const TexelFormat& format, const uint8* bytes, double out[4]) noexcept {
    out[0] = out[1] = out[2] = 0.0;
    out[3] = 1.0;
    for (uint32 k = 0; k < format.components; ++k) {
        switch (format.kind) {
        case TexelKind::Unorm8:
            out[k] = format.format == PixelFormat::RGBA8Snorm
                         ? fmax(static_cast<int8>(bytes[k]) / 127.0, -1.0)
                         : bytes[k] / 255.0;
            break;
        case TexelKind::Srgb8:
            out[k] = k < 3 ? SrgbDecode(bytes[k] / 255.0) : bytes[k] / 255.0;
            break;
        case TexelKind::Half: {
            uint16 bits = 0;
            memcpy(&bits, bytes + k * 2u, 2);
            out[k] = Float16::FromBits(bits).ToFloat();
            break;
        }

        case TexelKind::Float32: {
            float value = 0.0f;
            memcpy(&value, bytes + k * 4u, 4);
            out[k] = value;
            break;
        }

        case TexelKind::Uint32: {
            uint32 value = 0;
            memcpy(&value, bytes + k * 4u, 4);
            out[k] = value;
            break;
        }

        case TexelKind::Sint32: {
            int32 value = 0;
            memcpy(&value, bytes + k * 4u, 4);
            out[k] = value;
            break;
        }

        case TexelKind::SmallFloat: {
            uint32 packed = 0;
            memcpy(&packed, bytes, 4);
            const uint32 shift = k == 0 ? 0u : (k == 1 ? 11u : 22u);
            const uint32 width = k == 2 ? 10u : 11u;
            out[k] = DecodeSmallFloat((packed >> shift) & ((1u << width) - 1u), width - 5u);
            break;
        }
        }
    }
}

// 기대 값 reference와 읽은 값 actual이 형식 정밀도 안에서 같은가(8 bit는 ±1 부호).
bool TexelNear(const TexelFormat& format, double reference, double actual) noexcept {
    switch (format.kind) {
    case TexelKind::Unorm8:
        // 8 bit 부호 ±1(snorm은 127 단계)이다.
        return fabs(reference - actual) <=
               (format.format == PixelFormat::RGBA8Snorm ? 1.0 / 127.0 : 1.0 / 255.0) + 1.0e-9;
    case TexelKind::Srgb8:
        return fabs(SrgbEncode(reference) - SrgbEncode(actual)) <= 1.0 / 255.0 + 1.0e-9;
    case TexelKind::Half:
        return fabs(reference - actual) <= fabs(reference) * 1.0e-3 + 1.0e-4;
    case TexelKind::SmallFloat:
        return fabs(reference - actual) <= fabs(reference) * 3.2e-2 + 1.0e-4;
    case TexelKind::Float32:
        return fabs(reference - actual) <= fabs(reference) * 2.0e-6 + 1.0e-7;
    default:
        return reference == actual;
    }
}

bool ReadMip(Device& device, TextureHandle texture, const TextureDesc& desc, uint32 mip,
             Array<uint8>& bytes) noexcept {
    const uint64 size = TextureMipPlaneBytes(desc, mip, 0);
    return size != 0 && bytes.TryResize(static_cast<usize>(size)) &&
           device.ReadTexture(texture, mip, 0, bytes.View()).IsOk();
}

// --- 밉 생성 ---

// 축 하나의 면적 박스 footprint(GenerateMips.rsf와 같은 정의)다.
void Footprint(uint32 x, uint32 source, uint32 destination, uint32 taps[3],
               double weights[3]) noexcept {
    const uint32 first = source == 1 ? 0u : x * 2u;
    for (uint32 k = 0; k < 3; ++k) {
        taps[k] = first + k < source ? first + k : source - 1;
    }

    if (source == 1) {
        weights[0] = 1.0;
        weights[1] = weights[2] = 0.0;
    } else if ((source & 1u) == 0) {
        weights[0] = weights[1] = 0.5;
        weights[2] = 0.0;
    } else {
        weights[0] = (destination - static_cast<double>(x)) / source;
        weights[1] = static_cast<double>(destination) / source;
        weights[2] = (x + 1.0) / source;
    }
}

// mip k + 1을 GPU가 쓴 mip k의 CPU 박스와 대조한다(틀린 texel 수).
uint32 CompareNextMip(const TexelFormat& format, const TextureDesc& desc, uint32 mip,
                      const Array<uint8>& source, const Array<uint8>& destination) noexcept {
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    const uint32 sw = MipExtent(desc.width, mip);
    const uint32 sh = MipExtent(desc.height, mip);
    const uint32 sd = volume ? MipExtent(desc.depth, mip) : 1u;
    const uint32 dw = MipExtent(desc.width, mip + 1);
    const uint32 dh = MipExtent(desc.height, mip + 1);
    const uint32 dd = volume ? MipExtent(desc.depth, mip + 1) : 1u;
    const uint32 slices = volume ? 1u : TextureSliceCount(desc);
    uint32 wrong = 0;
    for (uint32 slice = 0; slice < slices; ++slice) {
        for (uint32 z = 0; z < dd; ++z) {
            uint32 tz[3] = {};
            double wz[3] = {1.0, 0.0, 0.0};
            if (volume) {
                Footprint(z, sd, dd, tz, wz);
            }

            for (uint32 y = 0; y < dh; ++y) {
                uint32 ty[3] = {};
                double wy[3] = {};
                Footprint(y, sh, dh, ty, wy);
                for (uint32 x = 0; x < dw; ++x) {
                    uint32 tx[3] = {};
                    double wx[3] = {};
                    Footprint(x, sw, dw, tx, wx);
                    double expected[4] = {};
                    for (uint32 c = 0; c < 3; ++c) {
                        for (uint32 b = 0; b < 3; ++b) {
                            for (uint32 a = 0; a < 3; ++a) {
                                const double weight = wx[a] * wy[b] * wz[c];
                                if (weight == 0.0) {
                                    continue;
                                }

                                const usize texel =
                                    ((static_cast<usize>(slice + (volume ? tz[c] : 0u)) * sh +
                                      ty[b]) *
                                         sw +
                                     tx[a]) *
                                    format.bytes;
                                double value[4] = {};
                                DecodeTexel(format, source.Data() + texel, value);
                                for (uint32 k = 0; k < 4; ++k) {
                                    expected[k] += weight * value[k];
                                }
                            }
                        }
                    }

                    const usize texel =
                        ((static_cast<usize>(slice + z) * dh + y) * dw + x) * format.bytes;
                    double actual[4] = {};
                    DecodeTexel(format, destination.Data() + texel, actual);
                    for (uint32 k = 0; k < format.components; ++k) {
                        if (!TexelNear(format, expected[k], actual[k])) {
                            if (wrong == 0) {
                                printf("    mip %u texel (%u, %u, %u) slice %u channel %u: %.6f, "
                                       "expected %.6f\n",
                                       mip + 1, x, y, z, slice, k, actual[k], expected[k]);
                            }

                            ++wrong;
                        }
                    }
                }
            }
        }
    }

    return wrong;
}

// mip 0을 slice마다 MipPatternPixel로 그린다(D3D 장치는 렌더 타깃·storage 텍스처의 UpdateTexture를
// 받지 않는다 — 그린 값은 형식으로 양자화되고 비교는 장치가 쓴 mip을 읽어 한다). float 형식은 [0,
// 4)다.
bool DrawMipZero(Device& device, render::ShaderSet& set, render::PipelineCache& cache,
                 TextureHandle texture, const TextureDesc& desc,
                 const TexelFormat& format) noexcept {
    auto entry = set.FindEntry(shaders::MipPatternProbe.nameHash);
    auto request = entry ? render::FullscreenPipelineRequest(set, entry.Value(), desc.format)
                         : Result<render::GraphicsPipelineRequest>::Failure(entry.Error());
    if (!request) {
        return false;
    }

    request.Value().state.drawConstantBytes = sizeof(engine_shaders::ProbeConstants);
    auto pipeline = cache.FindOrCreate(request.Value());
    if (!pipeline) {
        return false;
    }

    const bool normalized = format.kind == TexelKind::Unorm8 || format.kind == TexelKind::Srgb8;
    const uint32 slices =
        desc.dimension == TextureDimension::Texture3D ? desc.depth : TextureSliceCount(desc);
    for (uint32 slice = 0; slice < slices; ++slice) {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = texture;
        pass.colors[0].load = LoadAction::DontCare;
        pass.colors[0].slice = slice;
        auto frame = device.BeginPass(pass);
        if (!frame) {
            return false;
        }

        engine_shaders::ProbeConstants constants;
        constants.control.y = slice;
        constants.parameters.x = normalized ? 1.0f : 4.0f;
        frame.Value()->SetPipeline(pipeline.Value());
        const bool set = frame.Value()->SetDrawConstants(DrawConstantBytes(constants)).IsOk();
        render::DrawFullscreenTriangle(*frame.Value());
        if (!device.EndFrame() || !set) {
            return false;
        }
    }

    return true;
}

struct MipCase {
    const char* label;
    TextureDimension dimension;
    uint32 width;
    uint32 height;
    uint32 depth;
    uint32 arraySize;
};

constexpr MipCase MipCases[] = {{"2D 13x7", TextureDimension::Texture2D, 13, 7, 1, 1},
                                {"2D array 9x6x3", TextureDimension::Texture2DArray, 9, 6, 1, 3},
                                {"cube 8", TextureDimension::TextureCube, 8, 8, 1, 1},
                                {"3D 5x6x7", TextureDimension::Texture3D, 5, 6, 7, 1}};

constexpr PixelFormat MipFormats[] = {PixelFormat::RGBA8Unorm, PixelFormat::RGBA8UnormSrgb,
                                      PixelFormat::RGBA16Float, PixelFormat::RGBA32Float,
                                      PixelFormat::R32Float};

void CheckGenerateMips(Device& device, render::ShaderSet& set, render::PipelineCache& cache,
                       Allocator& allocator) noexcept {
    uint32 runs = 0;
    uint32 computeRuns = 0;
    uint32 rasterRuns = 0;
    for (const PixelFormat pixelFormat : MipFormats) {
        const TexelFormat& format = *FindTexelFormat(pixelFormat);
        for (const MipCase& shape : MipCases) {
            TextureDesc desc;
            desc.dimension = shape.dimension;
            desc.width = shape.width;
            desc.height = shape.height;
            desc.depth = shape.depth;
            desc.arraySize = shape.arraySize;
            desc.format = pixelFormat;
            desc.mipLevels = TextureFullMipCount(desc);
            const bool storable = shape.dimension != TextureDimension::TextureCube &&
                                  render::StorageImageFormatOf(pixelFormat).IsOk();
            desc.usage = storable ? TextureUsage::RenderTarget | TextureUsage::Storage
                                  : TextureUsage::RenderTarget;
            Check(render::SelectMipGenerationPath(device, desc).IsOk() ||
                      !HasPixelFormatCaps(device.GetPixelFormatCaps(pixelFormat),
                                          PixelFormatCaps::RenderTarget),
                  "a render-target-capable format has a mip generation path");
            for (uint32 path = 0; path < 2; ++path) {
                const render::MipGenerationPath which = path == 0
                                                            ? render::MipGenerationPath::Compute
                                                            : render::MipGenerationPath::Raster;
                if (!render::CanGenerateMips(device, desc, which)) {
                    continue;
                }

                auto texture = device.CreateTexture(desc, {});
                if (!texture || !DrawMipZero(device, set, cache, texture.Value(), desc, format)) {
                    Check(false, "mip generation texture and mip 0 pattern");
                    PrintDeviceError(device);
                    if (texture) {
                        device.Retire(texture.Value());
                    }

                    continue;
                }

                auto generated =
                    render::GenerateMips(device, set, cache, texture.Value(), desc, which);
                char what[160];
                snprintf(what, sizeof(what), "%s %s %s mips generate", PixelFormatName(pixelFormat),
                         shape.label, path == 0 ? "compute" : "raster");
                Check(generated.IsOk(), what);
                if (!generated) {
                    printf("    %s\n", ErrorCodeName(generated.Error()));
                    PrintDeviceError(device);
                }

                Array<uint8> previous(allocator);
                Array<uint8> current(allocator);
                uint32 wrong = 0;
                bool read = generated.IsOk() && ReadMip(device, texture.Value(), desc, 0, previous);
                for (uint32 mip = 0; read && mip + 1 < desc.mipLevels; ++mip) {
                    read = ReadMip(device, texture.Value(), desc, mip + 1, current);
                    if (read) {
                        wrong += CompareNextMip(format, desc, mip, previous, current);
                        read = previous.TryResize(current.Size()).IsOk();
                        if (read) {
                            platform::MemoryCopy(previous.Data(), current.Data(), current.Size());
                        }
                    }
                }

                snprintf(what, sizeof(what), "%s %s %s mips = CPU area box of the previous mip",
                         PixelFormatName(pixelFormat), shape.label,
                         path == 0 ? "compute" : "raster");
                Check(!generated || (read && wrong == 0), what);
                device.Retire(texture.Value());
                ++runs;
                computeRuns += path == 0 ? 1u : 0u;
                rasterRuns += path == 1 ? 1u : 0u;
            }
        }
    }

    printf("  %s generate mips: %u runs (compute %u, raster %u)\n", backendName, runs, computeRuns,
           rasterRuns);
}

// --- UAV clear ---

struct ClearCase {
    PixelFormat format;
    render::ClearValue first;
    render::ClearValue second;
    double firstValue[4];
    double secondValue[4];
};

void CheckWritableClears(Device& device, render::ShaderSet& set, render::PipelineCache& cache,
                         Allocator& allocator) noexcept {
    const ClearCase cases[] = {{PixelFormat::RGBA8Unorm,
                                render::ClearValue::Float(0.2f, 0.4f, 0.6f, 0.8f),
                                render::ClearValue::Float(1.0f, 0.0f, 1.0f, 0.0f),
                                {0.2, 0.4, 0.6, 0.8},
                                {1, 0, 1, 0}},
                               {PixelFormat::RGBA16Float,
                                render::ClearValue::Float(0.25f, -1.5f, 2.0f, 1.0f),
                                render::ClearValue::Float(4.0f, 0.5f, -0.25f, 0.0f),
                                {0.25, -1.5, 2, 1},
                                {4, 0.5, -0.25, 0}},
                               {PixelFormat::RGBA32Float,
                                render::ClearValue::Float(0.1f, 0.2f, 0.3f, 0.4f),
                                render::ClearValue::Float(-7.0f, 1.0e6f, 3.5f, 0.0f),
                                {static_cast<double>(0.1f), static_cast<double>(0.2f),
                                 static_cast<double>(0.3f), static_cast<double>(0.4f)},
                                {-7, 1.0e6, 3.5, 0}},
                               {PixelFormat::R32Float,
                                render::ClearValue::Float(0.75f, 0, 0, 0),
                                render::ClearValue::Float(-2.0f, 0, 0, 0),
                                {0.75, 0, 0, 1},
                                {-2, 0, 0, 1}},
                               {PixelFormat::R32Uint,
                                render::ClearValue::Uint(7, 0, 0, 0),
                                render::ClearValue::Uint(0xdeadbeefu, 0, 0, 0),
                                {7, 0, 0, 1},
                                {3735928559.0, 0, 0, 1}},
                               {PixelFormat::R32Sint,
                                render::ClearValue::Sint(-5, 0, 0, 0),
                                render::ClearValue::Sint(123456, 0, 0, 0),
                                {-5, 0, 0, 1},
                                {123456, 0, 0, 1}},
                               {PixelFormat::RGBA32Uint,
                                render::ClearValue::Uint(1, 2, 3, 4),
                                render::ClearValue::Uint(9, 8, 7, NumericLimits<uint32>::Max()),
                                {1, 2, 3, 4},
                                {9, 8, 7, 4294967295.0}},
                               {PixelFormat::RG16Float,
                                render::ClearValue::Float(0.5f, 2.0f, 0, 0),
                                render::ClearValue::Float(-1.0f, 0.125f, 0, 0),
                                {0.5, 2, 0, 1},
                                {-1, 0.125, 0, 1}},
                               {PixelFormat::R16Float,
                                render::ClearValue::Float(3.0f, 0, 0, 0),
                                render::ClearValue::Float(-0.5f, 0, 0, 0),
                                {3, 0, 0, 1},
                                {-0.5, 0, 0, 1}},
                               {PixelFormat::RG32Float,
                                render::ClearValue::Float(1.5f, -2.5f, 0, 0),
                                render::ClearValue::Float(8.0f, 16.0f, 0, 0),
                                {1.5, -2.5, 0, 1},
                                {8, 16, 0, 1}},
                               {PixelFormat::RG11B10Ufloat,
                                render::ClearValue::Float(0.5f, 2.0f, 0.25f, 0),
                                render::ClearValue::Float(4.0f, 0.125f, 1.0f, 0),
                                {0.5, 2, 0.25, 1},
                                {4, 0.125, 1, 1}}};
    const TextureDimension dimensions[] = {
        TextureDimension::Texture2D, TextureDimension::Texture2DArray, TextureDimension::Texture3D};
    uint32 runs = 0;
    for (const ClearCase& clear : cases) {
        const TexelFormat& format = *FindTexelFormat(clear.format);
        if (!HasPixelFormatCaps(device.GetPixelFormatCaps(clear.format),
                                PixelFormatCaps::StorageWrite | PixelFormatCaps::Sampled)) {
            continue;
        }

        for (const TextureDimension dimension : dimensions) {
            TextureDesc desc;
            desc.dimension = dimension;
            desc.width = 12;
            desc.height = 10;
            desc.depth = dimension == TextureDimension::Texture3D ? 4u : 1u;
            desc.arraySize = dimension == TextureDimension::Texture2DArray ? 3u : 1u;
            desc.format = clear.format;
            desc.usage = TextureUsage::Storage;
            auto texture = device.CreateTexture(desc, {});
            TextureViewDesc writable;
            writable.access = ViewAccess::ReadWrite;
            writable.mipCount = 1;
            auto view = texture ? device.CreateTextureView(texture.Value(), writable)
                                : Result<TextureHandle>::Failure(texture.Error());
            char what[160];
            snprintf(what, sizeof(what), "%s clear target (dimension %u)",
                     PixelFormatName(clear.format), static_cast<uint32>(dimension));
            Check(texture && view, what);
            if (!texture || !view) {
                PrintDeviceError(device);
                if (texture) {
                    device.Retire(texture.Value());
                }

                continue;
            }

            const bool flat = dimension == TextureDimension::Texture2D;
            const uint32 layers = flat ? 1u : (desc.depth > 1 ? desc.depth : desc.arraySize);
            render::WritableTextureRegion whole;
            whole.width = desc.width;
            whole.height = desc.height;
            whole.depth = layers;
            render::WritableTextureRegion part;
            part.x = 3;
            part.y = 2;
            part.z = flat ? 0u : 1u;
            part.width = 5;
            part.height = 4;
            part.depth = flat ? 1u : 2u;
            auto frame = device.BeginPass(PassDesc{});
            Result<void> first = Result<void>::Failure(ErrorCode::InvalidArgument);
            Result<void> second = Result<void>::Failure(ErrorCode::InvalidArgument);
            if (frame) {
                first = render::ClearWritableTexture(device, *frame.Value(), set, cache,
                                                     view.Value(), clear.first, whole);
                second = render::ClearWritableTexture(device, *frame.Value(), set, cache,
                                                      view.Value(), clear.second, part);
                (void)device.EndFrame();
            }

            snprintf(what, sizeof(what), "%s clear dispatches (dimension %u)",
                     PixelFormatName(clear.format), static_cast<uint32>(dimension));
            Check(frame && first && second, what);
            Array<uint8> bytes(allocator);
            uint32 wrong = 0;
            if (frame && first && second && ReadMip(device, texture.Value(), desc, 0, bytes)) {
                for (uint32 z = 0; z < layers; ++z) {
                    for (uint32 y = 0; y < desc.height; ++y) {
                        for (uint32 x = 0; x < desc.width; ++x) {
                            const bool inside = x >= part.x && x < part.x + part.width &&
                                                y >= part.y && y < part.y + part.height &&
                                                z >= part.z && z < part.z + part.depth;
                            const double* expected = inside ? clear.secondValue : clear.firstValue;
                            double actual[4] = {};
                            DecodeTexel(
                                format,
                                bytes.Data() +
                                    ((static_cast<usize>(z) * desc.height + y) * desc.width + x) *
                                        format.bytes,
                                actual);
                            for (uint32 k = 0; k < format.components; ++k) {
                                wrong += TexelNear(format, expected[k], actual[k]) ? 0u : 1u;
                            }
                        }
                    }
                }
            } else {
                wrong = 1;
                PrintDeviceError(device);
            }

            snprintf(what, sizeof(what),
                     "%s cleared texels match inside and outside the region (dimension %u)",
                     PixelFormatName(clear.format), static_cast<uint32>(dimension));
            Check(wrong == 0, what);
            device.Retire(view.Value());
            device.Retire(texture.Value());
            ++runs;
        }
    }

    // typed 버퍼 clear: RGBA32Float 원소 [10, 90)을 값으로(나머지 0).
    constexpr uint32 elements = 100;
    BufferDesc bufferDesc;
    bufferDesc.size = elements * 16u;
    bufferDesc.usage = BufferUsage::ShaderRead | BufferUsage::ShaderWrite | BufferUsage::Raw;
    Array<uint8> zeros(allocator);
    bool ok = zeros.TryResize(bufferDesc.size).IsOk();
    if (ok) {
        platform::MemorySet(zeros.Data(), 0, zeros.Size());
    }

    auto buffer = ok ? device.CreateBuffer(bufferDesc, zeros.View())
                     : Result<BufferHandle>::Failure(ErrorCode::OutOfMemory);
    BufferViewDesc typed;
    typed.kind = BufferViewKind::Typed;
    typed.access = ViewAccess::ReadWrite;
    typed.format = PixelFormat::RGBA32Float;
    auto bufferView = buffer ? device.CreateBufferView(buffer.Value(), typed)
                             : Result<BufferViewHandle>::Failure(buffer.Error());
    if (buffer && bufferView) {
        auto frame = device.BeginPass(PassDesc{});
        Result<void> cleared = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (frame) {
            cleared = render::ClearWritableBuffer(
                device, *frame.Value(), set, cache, bufferView.Value(), PixelFormat::RGBA32Float,
                render::ClearValue::Float(1.0f, 2.0f, 3.0f, 4.0f), 10, 80);
            (void)device.EndFrame();
        }

        Array<float> values(allocator);
        const bool read =
            values.TryResize(elements * 4u).IsOk() &&
            device
                .ReadBuffer(buffer.Value(), 0,
                            Span<uint8>(reinterpret_cast<uint8*>(values.Data()), bufferDesc.size))
                .IsOk();
        uint32 wrong = read ? 0u : 1u;
        for (uint32 element = 0; read && element < elements; ++element) {
            const bool inside = element >= 10 && element < 90;
            for (uint32 k = 0; k < 4; ++k) {
                wrong += values[element * 4u + k] != (inside ? 1.0f + k : 0.0f) ? 1u : 0u;
            }
        }

        Check(cleared && wrong == 0, "typed buffer clear writes [10, 90) and keeps the rest");
        if (!cleared) {
            PrintDeviceError(device);
        }
    } else {
        Check(false, "typed clear buffer and view");
        PrintDeviceError(device);
    }

    if (bufferView) {
        device.Retire(bufferView.Value());
    }

    if (buffer) {
        device.Retire(buffer.Value());
    }

    printf("  %s writable clears: %u texture runs\n", backendName, runs);
}

// --- 부분 clear·간접 인자 ---

constexpr uint32 RegionTarget = 32;
constexpr ScissorRect RegionRect = {5, 7, 10, 6};

bool InsideRegion(uint32 x, uint32 y) noexcept {
    return x >= RegionRect.x && x < RegionRect.x + RegionRect.width && y >= RegionRect.y &&
           y < RegionRect.y + RegionRect.height;
}

void CheckRegionClears(Device& device, render::ShaderSet& set, render::PipelineCache& cache,
                       Allocator& allocator) noexcept {
    // 색 float + 깊이를 한 번에, 색 uint, 깊이만(DepthOnlyPixel).
    TextureDesc colorDesc;
    colorDesc.width = colorDesc.height = RegionTarget;
    colorDesc.format = PixelFormat::RGBA8Unorm;
    colorDesc.usage = TextureUsage::RenderTarget;
    TextureDesc uintDesc = colorDesc;
    uintDesc.format = PixelFormat::R32Uint;
    TextureDesc depthDesc = colorDesc;
    depthDesc.format = PixelFormat::D32Float;
    depthDesc.usage = TextureUsage::DepthStencil;
    auto color = device.CreateTexture(colorDesc, {});
    auto integer = device.CreateTexture(uintDesc, {});
    auto depth = device.CreateTexture(depthDesc, {});
    Check(color && integer && depth, "region clear targets");
    if (color && integer && depth) {
        PassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].texture = color.Value();
        pass.colors[0].clear = {0.0f, 0.0f, 0.0f, 1.0f};
        pass.depth.texture = depth.Value();
        pass.depth.clearDepth = 0.25f;
        auto frame = device.BeginPass(pass);
        Result<void> cleared = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (frame) {
            render::ClearRegionDesc desc;
            desc.rect = RegionRect;
            desc.targetWidth = desc.targetHeight = RegionTarget;
            desc.colorFormat = PixelFormat::RGBA8Unorm;
            desc.clearColor = true;
            desc.color = render::ClearValue::Float(1.0f, 0.4f, 0.0f, 1.0f);
            desc.depthFormat = PixelFormat::D32Float;
            desc.clearDepth = true;
            desc.depth = 0.75f;
            cleared = render::ClearRegion(*frame.Value(), set, cache, desc);
            (void)device.EndFrame();
        }

        Check(cleared.IsOk(), "color and depth region clear records");
        Array<uint8> pixels(allocator);
        Array<uint8> depths(allocator);
        uint32 wrong = 0;
        if (cleared && ReadMip(device, color.Value(), colorDesc, 0, pixels) &&
            ReadMip(device, depth.Value(), depthDesc, 0, depths)) {
            for (uint32 y = 0; y < RegionTarget; ++y) {
                for (uint32 x = 0; x < RegionTarget; ++x) {
                    const uint8* p = pixels.Data() + (y * RegionTarget + x) * 4u;
                    const bool inside = InsideRegion(x, y);
                    const uint8 expected[4] = {static_cast<uint8>(inside ? 255 : 0),
                                               static_cast<uint8>(inside ? 102 : 0), 0, 255};
                    wrong += memcmp(p, expected, 4) == 0 ? 0u : 1u;
                    float d = 0.0f;
                    memcpy(&d, depths.Data() + (y * RegionTarget + x) * 4u, 4);
                    wrong += d == (inside ? 0.75f : 0.25f) ? 0u : 1u;
                }
            }
        } else {
            wrong = 1;
            PrintDeviceError(device);
        }

        Check(wrong == 0, "color and depth region clear changes only the rectangle");

        // 색 uint와 깊이만 지우기(색 attachment 없음 — DepthOnlyPixel).
        PassDesc integerPass;
        integerPass.colorCount = 1;
        integerPass.colors[0].texture = integer.Value();
        integerPass.colors[0].clear = {0.0f, 0.0f, 0.0f, 0.0f};
        frame = device.BeginPass(integerPass);
        cleared = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (frame) {
            render::ClearRegionDesc desc;
            desc.rect = RegionRect;
            desc.targetWidth = desc.targetHeight = RegionTarget;
            desc.colorFormat = PixelFormat::R32Uint;
            desc.clearColor = true;
            desc.color = render::ClearValue::Uint(0xabcdef01u, 0, 0, 0);
            cleared = render::ClearRegion(*frame.Value(), set, cache, desc);
            (void)device.EndFrame();
        }

        PassDesc depthPass;
        depthPass.colorCount = 0;
        depthPass.depth.texture = depth.Value();
        depthPass.depth.clearDepth = 0.5f;
        auto depthFrame = device.BeginPass(depthPass);
        Result<void> depthCleared = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (depthFrame) {
            render::ClearRegionDesc desc;
            desc.rect = RegionRect;
            desc.targetWidth = desc.targetHeight = RegionTarget;
            desc.depthFormat = PixelFormat::D32Float;
            desc.clearDepth = true;
            desc.depth = 1.0f;
            depthCleared = render::ClearRegion(*depthFrame.Value(), set, cache, desc);
            (void)device.EndFrame();
        }

        wrong = 0;
        if (cleared && depthCleared && ReadMip(device, integer.Value(), uintDesc, 0, pixels) &&
            ReadMip(device, depth.Value(), depthDesc, 0, depths)) {
            for (uint32 y = 0; y < RegionTarget; ++y) {
                for (uint32 x = 0; x < RegionTarget; ++x) {
                    uint32 value = 0;
                    float d = 0.0f;
                    memcpy(&value, pixels.Data() + (y * RegionTarget + x) * 4u, 4);
                    memcpy(&d, depths.Data() + (y * RegionTarget + x) * 4u, 4);
                    const bool inside = InsideRegion(x, y);
                    wrong += value == (inside ? 0xabcdef01u : 0u) ? 0u : 1u;
                    wrong += d == (inside ? 1.0f : 0.5f) ? 0u : 1u;
                }
            }
        } else {
            wrong = 1;
            PrintDeviceError(device);
        }

        Check(cleared && depthCleared && wrong == 0,
              "uint color and depth-only region clears change only the rectangle");
    }

    // 스텐실(깊이 + 스텐실 형식, plane 1 readback이 되는 장치에서).
    TextureDesc stencilDesc = depthDesc;
    stencilDesc.format = PixelFormat::D32FloatS8Uint;
    auto stencil = HasPixelFormatCaps(device.GetPixelFormatCaps(stencilDesc.format),
                                      PixelFormatCaps::DepthStencil)
                       ? device.CreateTexture(stencilDesc, {})
                       : Result<TextureHandle>::Failure(ErrorCode::Unsupported);
    if (stencil) {
        PassDesc pass;
        pass.colorCount = 0;
        pass.depth.texture = stencil.Value();
        pass.depth.clearDepth = 0.0f;
        pass.depth.clearStencil = 3;
        auto frame = device.BeginPass(pass);
        Result<void> cleared = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (frame) {
            render::ClearRegionDesc desc;
            desc.rect = RegionRect;
            desc.targetWidth = desc.targetHeight = RegionTarget;
            desc.depthFormat = stencilDesc.format;
            desc.clearStencil = true;
            desc.stencil = 0x5a;
            cleared = render::ClearRegion(*frame.Value(), set, cache, desc);
            (void)device.EndFrame();
        }

        Array<uint8> stencils(allocator);
        const uint64 planeBytes = TextureMipPlaneBytes(stencilDesc, 0, 1);
        const bool read = cleared && stencils.TryResize(static_cast<usize>(planeBytes)).IsOk() &&
                          device.ReadTexture(stencil.Value(), 0, 1, stencils.View()).IsOk();
        if (read) {
            const uint32 stride = static_cast<uint32>(planeBytes / (RegionTarget * RegionTarget));
            uint32 wrong = 0;
            for (uint32 y = 0; y < RegionTarget; ++y) {
                for (uint32 x = 0; x < RegionTarget; ++x) {
                    const uint8 value = stencils[(y * RegionTarget + x) * stride];
                    wrong += value == (InsideRegion(x, y) ? 0x5a : 3) ? 0u : 1u;
                }
            }

            Check(wrong == 0, "stencil region clear changes only the rectangle");
        } else {
            Check(cleared.IsOk(), "stencil region clear records");
            printf("  %s stencil region clear: stencil plane readback unavailable (checked the "
                   "recording only)\n",
                   backendName);
        }

        device.Retire(stencil.Value());
    }

    // 간접 인자 초기화: byte 4부터 기록 셋 (7, 8, 9).
    BufferDesc arguments;
    arguments.size = 64;
    arguments.usage = BufferUsage::Indirect | BufferUsage::ShaderWrite | BufferUsage::Raw;
    uint8 zeros[64] = {};
    auto buffer = device.CreateBuffer(arguments, Span<const uint8>(zeros, sizeof(zeros)));
    BufferViewDesc raw;
    raw.kind = BufferViewKind::Raw;
    raw.access = ViewAccess::ReadWrite;
    auto view = buffer ? device.CreateBufferView(buffer.Value(), raw)
                       : Result<BufferViewHandle>::Failure(buffer.Error());
    if (buffer && view) {
        auto frame = device.BeginPass(PassDesc{});
        Result<void> initialized = Result<void>::Failure(ErrorCode::InvalidArgument);
        if (frame) {
            DispatchIndirectArguments values;
            values.groupCountX = 7;
            values.groupCountY = 8;
            values.groupCountZ = 9;
            initialized = render::InitializeDispatchArguments(device, *frame.Value(), set, cache,
                                                              view.Value(), 4, 3, values);
            (void)device.EndFrame();
        }

        uint32 words[16] = {};
        const bool read =
            initialized &&
            device.ReadBuffer(buffer.Value(), 0, Span<uint8>(reinterpret_cast<uint8*>(words), 64))
                .IsOk();
        bool match = read && words[0] == 0;
        for (uint32 index = 1; match && index < 16; ++index) {
            const uint32 expected = index <= 9 ? 7u + (index - 1) % 3u : 0u;
            match = words[index] == expected;
        }

        Check(match, "dispatch argument initialization writes three records from byte 4");
        if (!initialized) {
            PrintDeviceError(device);
        }
    } else {
        Check(false, "dispatch argument buffer and view");
    }

    if (view) {
        device.Retire(view.Value());
    }

    if (buffer) {
        device.Retire(buffer.Value());
    }

    const TextureHandle targets[3] = {color ? color.Value() : TextureHandle{},
                                      integer ? integer.Value() : TextureHandle{},
                                      depth ? depth.Value() : TextureHandle{}};
    for (const TextureHandle target : targets) {
        if (!target.IsNull()) {
            device.Retire(target);
        }
    }
}

// --- 기본 텍스처 캐시 ---

void CheckDefaultTextures(Device& device, Allocator& allocator) noexcept {
    struct DefaultCase {
        PixelFormat format;
        ClearColor value;
    };

    const DefaultCase cases[] = {{PixelFormat::RGBA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}},
                                 {PixelFormat::RGBA8UnormSrgb, {0.5f, 0.0f, 1.0f, 0.5f}},
                                 {PixelFormat::RGBA16Float, {-2.0f, 0.5f, 65504.0f, 1.0f}},
                                 {PixelFormat::RGBA32Float, {0.1f, -0.2f, 3.0e8f, 0.0f}},
                                 {PixelFormat::R32Uint, {7.0f, 0, 0, 0}},
                                 {PixelFormat::R32Sint, {-3.0f, 0, 0, 0}},
                                 {PixelFormat::RG16Float, {0.25f, -0.75f, 0, 0}}};
    const TextureDimension dimensions[] = {
        TextureDimension::Texture2D, TextureDimension::Texture2DArray, TextureDimension::Texture3D,
        TextureDimension::TextureCube, TextureDimension::TextureCubeArray};
    render::DefaultTextureCache cache;
    uint32 checked = 0;
    for (const DefaultCase& value : cases) {
        uint8 texel[render::MaxDefaultTexelBytes] = {};
        auto encoded = render::EncodeDefaultTexel(value.format, value.value,
                                                  Span<uint8>(texel, sizeof(texel)));
        Check(encoded.IsOk(), "default texel encodes");
        for (const TextureDimension dimension : dimensions) {
            if (dimension == TextureDimension::TextureCubeArray &&
                !device.Caps().features.textureCubeArray) {
                Check(cache.Find(device, dimension, value.format, value.value).Error() ==
                          ErrorCode::Unsupported,
                      "cube array default texture is unsupported without the device feature");
                continue;
            }

            auto texture = cache.Find(device, dimension, value.format, value.value);
            auto again = cache.Find(device, dimension, value.format, value.value);
            char what[128];
            snprintf(what, sizeof(what), "%s default texture (dimension %u) is created once",
                     PixelFormatName(value.format), static_cast<uint32>(dimension));
            Check(texture && again && texture.Value() == again.Value(), what);
            if (!texture || !encoded) {
                PrintDeviceError(device);
                continue;
            }

            TextureDesc desc;
            desc.dimension = dimension;
            desc.width = desc.height = 1;
            desc.format = value.format;
            Array<uint8> bytes(allocator);
            bool match = ReadMip(device, texture.Value(), desc, 0, bytes);
            const usize slices = bytes.Size() / encoded.Value();
            for (usize slice = 0; match && slice < slices; ++slice) {
                match = memcmp(bytes.Data() + slice * encoded.Value(), texel, encoded.Value()) == 0;
            }

            snprintf(what, sizeof(what), "%s default texture (dimension %u) reads back its value",
                     PixelFormatName(value.format), static_cast<uint32>(dimension));
            Check(match, what);
            ++checked;
        }
    }

    auto other = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm,
                            {0.0f, 0.0f, 0.0f, 0.0f});
    auto first = cache.Find(device, TextureDimension::Texture2D, PixelFormat::RGBA8Unorm,
                            {0.2f, 0.4f, 0.6f, 0.8f});
    Check(other && first && other.Value() != first.Value(),
          "a different default value is a different texture");
    Check(cache.Find(device, TextureDimension::Texture2D, PixelFormat::BC1Unorm, {}).Error() ==
              ErrorCode::Unsupported,
          "a compressed default texture is unsupported");
    cache.Retire(device);
    Check(cache.Size() == 0, "retire empties the default texture cache");
    printf("  %s default textures: %u (format, dimension) values read back\n", backendName,
           checked);
}

// --- 깊이 복사·자리표시 ---

constexpr uint32 CopySize = 16;
constexpr ScissorRect CopyRect = {4, 4, 8, 8};

void CheckCopyDepthAndPlaceholders(Device& device, render::ShaderSet& set,
                                   render::PipelineCache& cache, Allocator& allocator) noexcept {
    TextureDesc depthDesc;
    depthDesc.width = depthDesc.height = CopySize;
    depthDesc.format = PixelFormat::D32Float;
    depthDesc.usage = TextureUsage::DepthStencil;
    depthDesc.sampler.filter = Filter::Point;
    TextureDesc colorDesc = depthDesc;
    colorDesc.format = PixelFormat::R32Float;
    colorDesc.usage = TextureUsage::RenderTarget;
    auto source = device.CreateTexture(depthDesc, {});
    auto copiedDepth = device.CreateTexture(depthDesc, {});
    auto copiedColor = device.CreateTexture(colorDesc, {});
    auto toColor = set.FindEntry(render::CopyDepthToColorPixelType.hash);
    auto toDepth = set.FindEntry(render::CopyDepthPixelType.hash);
    Check(source && copiedDepth && copiedColor && toColor && toDepth,
          "depth copy textures and shaders");
    if (source && copiedDepth && copiedColor && toColor && toDepth) {
        // 원본: 깊이 0.25, 사각형 안 0.75(부분 clear 도우미).
        PassDesc pass;
        pass.colorCount = 0;
        pass.depth.texture = source.Value();
        pass.depth.clearDepth = 0.25f;
        auto frame = device.BeginPass(pass);
        if (frame) {
            render::ClearRegionDesc desc;
            desc.rect = CopyRect;
            desc.targetWidth = desc.targetHeight = CopySize;
            desc.depthFormat = PixelFormat::D32Float;
            desc.clearDepth = true;
            desc.depth = 0.75f;
            Check(render::ClearRegion(*frame.Value(), set, cache, desc).IsOk(),
                  "depth copy source region");
            (void)device.EndFrame();
        }

        const uint32 slot = TextureSlotOf(set, toColor.Value(), render::CopyDepthSourceName);
        auto colorRequest =
            render::FullscreenPipelineRequest(set, toColor.Value(), PixelFormat::R32Float);
        auto colorPipeline = colorRequest ? cache.FindOrCreate(colorRequest.Value())
                                          : Result<PipelineHandle>::Failure(colorRequest.Error());
        auto depthRequest =
            render::FullscreenPipelineRequest(set, toDepth.Value(), PixelFormat::R32Float);
        Result<PipelineHandle> depthPipeline = Result<PipelineHandle>::Failure(ErrorCode::NotFound);
        if (depthRequest) {
            depthRequest.Value().state.colorTargetCount = 0;
            depthRequest.Value().state.colorFormats[0] = PixelFormat::Unknown;
            depthRequest.Value().state.depthFormat = PixelFormat::D32Float;
            depthRequest.Value().state.depthStencil.depthTest = true;
            depthRequest.Value().state.depthStencil.depthWrite = true;
            depthRequest.Value().state.depthStencil.depthCompare = CompareFunction::Always;
            depthPipeline = cache.FindOrCreate(depthRequest.Value());
        }

        Check(colorPipeline && depthPipeline, "depth copy pipelines");
        if (colorPipeline && depthPipeline) {
            frame = device.BeginOffscreen(copiedColor.Value(), {});
            if (frame) {
                frame.Value()->SetPipeline(colorPipeline.Value());
                frame.Value()->SetTexture(ShaderStage::Pixel, slot, source.Value());
                render::DrawFullscreenTriangle(*frame.Value());
                (void)device.EndFrame();
            }

            PassDesc depthPass;
            depthPass.colorCount = 0;
            depthPass.depth.texture = copiedDepth.Value();
            depthPass.depth.clearDepth = 0.0f;
            auto depthFrame = device.BeginPass(depthPass);
            const uint32 depthSlot =
                TextureSlotOf(set, toDepth.Value(), render::CopyDepthSourceName);
            if (depthFrame) {
                depthFrame.Value()->SetPipeline(depthPipeline.Value());
                depthFrame.Value()->SetTexture(ShaderStage::Pixel, depthSlot, source.Value());
                render::DrawFullscreenTriangle(*depthFrame.Value());
                (void)device.EndFrame();
            }

            Array<uint8> colors(allocator);
            Array<uint8> depths(allocator);
            uint32 wrong = 0;
            if (frame && depthFrame && ReadMip(device, copiedColor.Value(), colorDesc, 0, colors) &&
                ReadMip(device, copiedDepth.Value(), depthDesc, 0, depths)) {
                for (uint32 y = 0; y < CopySize; ++y) {
                    for (uint32 x = 0; x < CopySize; ++x) {
                        const bool inside = x >= CopyRect.x && x < CopyRect.x + CopyRect.width &&
                                            y >= CopyRect.y && y < CopyRect.y + CopyRect.height;
                        const float expected = inside ? 0.75f : 0.25f;
                        float c = 0.0f;
                        float d = 0.0f;
                        memcpy(&c, colors.Data() + (y * CopySize + x) * 4u, 4);
                        memcpy(&d, depths.Data() + (y * CopySize + x) * 4u, 4);
                        wrong += c == expected ? 0u : 1u;
                        wrong += d == expected ? 0u : 1u;
                    }
                }
            } else {
                wrong = 1;
                PrintDeviceError(device);
            }

            Check(wrong == 0, "depth copies to color and to depth equal the source depth");
        }
    }

    const TextureHandle created[3] = {source ? source.Value() : TextureHandle{},
                                      copiedDepth ? copiedDepth.Value() : TextureHandle{},
                                      copiedColor ? copiedColor.Value() : TextureHandle{}};
    for (const TextureHandle texture : created) {
        if (!texture.IsNull()) {
            device.Retire(texture);
        }
    }

    // 자리표시: 단색과 바둑판(RGBA8Unorm 32 × 32).
    TextureDesc targetDesc;
    targetDesc.width = targetDesc.height = RegionTarget;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    auto solid = set.FindEntry(render::SolidColorPixelType.hash);
    auto missing = set.FindEntry(render::MissingShaderPixelType.hash);
    Check(target && solid && missing, "placeholder target and shaders");
    if (target && solid && missing) {
        for (uint32 kind = 0; kind < 2; ++kind) {
            auto request = render::FullscreenPipelineRequest(
                set, kind == 0 ? solid.Value() : missing.Value(), PixelFormat::RGBA8Unorm);
            if (request) {
                // 두 엔트리가 같은 생성 상수 원문을 include한다(쓰지 않는 쪽은 컴파일러가 버린다).
                request.Value().state.drawConstantBytes = sizeof(render::SolidColorConstants);
            }

            auto pipeline = request ? cache.FindOrCreate(request.Value())
                                    : Result<PipelineHandle>::Failure(request.Error());
            auto frame = pipeline ? device.BeginOffscreen(target.Value(), {})
                                  : Result<CommandContext*>::Failure(pipeline.Error());
            if (frame) {
                frame.Value()->SetPipeline(pipeline.Value());
                if (kind == 0) {
                    render::SolidColorConstants constants;
                    constants.color = {0.2f, 0.4f, 0.6f, 0.8f};
                    (void)frame.Value()->SetDrawConstants(DrawConstantBytes(constants));
                }

                render::DrawFullscreenTriangle(*frame.Value());
                (void)device.EndFrame();
            }

            Array<uint8> pixels(allocator);
            uint32 wrong = 0;
            if (frame && ReadMip(device, target.Value(), targetDesc, 0, pixels)) {
                for (uint32 y = 0; y < RegionTarget; ++y) {
                    for (uint32 x = 0; x < RegionTarget; ++x) {
                        const uint32 cell = ((x / render::MissingShaderCellPixels) ^
                                             (y / render::MissingShaderCellPixels)) &
                                            1u;
                        const float* checker = cell == 0 ? render::MissingShaderFirstColor
                                                         : render::MissingShaderSecondColor;
                        const uint8 expected[4] = {
                            static_cast<uint8>(kind == 0 ? 51 : checker[0] * 255.0f),
                            static_cast<uint8>(kind == 0 ? 102 : checker[1] * 255.0f),
                            static_cast<uint8>(kind == 0 ? 153 : checker[2] * 255.0f),
                            static_cast<uint8>(kind == 0 ? 204 : checker[3] * 255.0f)};
                        wrong +=
                            memcmp(pixels.Data() + (y * RegionTarget + x) * 4u, expected, 4) == 0
                                ? 0u
                                : 1u;
                    }
                }
            } else {
                wrong = 1;
                PrintDeviceError(device);
            }

            Check(wrong == 0, kind == 0 ? "solid color placeholder fills the constant color"
                                        : "missing shader placeholder draws the 8 px checker");
        }
    }

    if (target) {
        device.Retire(target.Value());
    }
}

} // namespace

uint32 RunUtilityPassChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    render::ShaderSet* set = shaders::ActiveSet();
    if (set == nullptr) {
        printf("  [NOT RUN] %s utility passes: no active engine shader set\n", name);
        return 0;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    {
        render::PipelineCache cache(allocator, device);
        CheckGenerateMips(device, *set, cache, allocator);
        CheckWritableClears(device, *set, cache, allocator);
        CheckRegionClears(device, *set, cache, allocator);
        CheckDefaultTextures(device, allocator);
        CheckCopyDepthAndPlaceholders(device, *set, cache, allocator);
        (void)device.WaitIdle();
    }

    Check(device.ValidationErrorCount() == validationBefore,
          "no validation errors during the utility passes");
    printf("  [%s] %s utility passes (Docs/Design/EngineShaders 14.6 item 3): %u failure%s\n",
           failures == 0 ? "PASS" : "FAIL", name, failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
