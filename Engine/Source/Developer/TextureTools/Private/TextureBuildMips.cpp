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

// 텍스처 빌드의 mip 생성이다(Docs/Design/AssetSystem 12.5절 mip 규칙 — 12.18 TP-D6로
// TextureBuild.cpp에서 나눔).
namespace rocks::texture_build_detail {

namespace build_mips {

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

} // namespace build_mips

} // namespace rocks::texture_build_detail
